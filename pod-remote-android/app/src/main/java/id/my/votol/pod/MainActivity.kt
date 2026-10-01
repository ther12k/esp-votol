package id.my.votol.pod

import android.Manifest
import android.annotation.SuppressLint
import android.bluetooth.BluetoothAdapter
import android.bluetooth.BluetoothDevice
import android.bluetooth.BluetoothGatt
import android.bluetooth.BluetoothGattCallback
import android.bluetooth.BluetoothGattCharacteristic
import android.bluetooth.BluetoothGattDescriptor
import android.bluetooth.BluetoothManager
import android.bluetooth.BluetoothProfile
import android.bluetooth.le.ScanCallback
import android.bluetooth.le.ScanFilter
import android.bluetooth.le.ScanResult
import android.bluetooth.le.ScanSettings
import android.content.Context
import android.content.pm.PackageManager
import android.os.Build
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.util.Log
import android.widget.EditText
import android.widget.TextView
import android.widget.Toast
import androidx.appcompat.app.AppCompatActivity
import androidx.core.app.ActivityCompat
import androidx.core.content.ContextCompat
import com.google.android.material.button.MaterialButton
import com.google.android.material.card.MaterialCardView
import java.util.UUID

/**
 * VOTOL Pod Remote — direct BLE command link to the tft-dash display pod.
 *
 * Protocol (mirrors the pod firmware): service c9d01402-…, write
 * "CMD:SECRET" to c9d01403-…, replies arrive as notifications on
 * c9d01404-… ("OK DISARM" / "ERR KEY") alongside status frames
 * ("ARMED FON"). SECRET = the 32-hex pairing key shown as a QR in the
 * pod's SYS → SET tab.
 */
class MainActivity : AppCompatActivity() {

    private val svcUuid: UUID = UUID.fromString("c9d01402-a1b2-4c3d-8e9f-aabbccddeeff")
    private val cmdUuid: UUID = UUID.fromString("c9d01403-a1b2-4c3d-8e9f-aabbccddeeff")
    private val statUuid: UUID = UUID.fromString("c9d01404-a1b2-4c3d-8e9f-aabbccddeeff")
    private val cccUuid: UUID = UUID.fromString("00002902-0000-1000-8000-00805f9b34fb")

    private val main = Handler(Looper.getMainLooper())
    private var adapter: BluetoothAdapter? = null
    private var gatt: BluetoothGatt? = null
    private var cmdChar: BluetoothGattCharacteristic? = null
    private var statChar: BluetoothGattCharacteristic? = null
    private var scanning = false
    private var armed: Boolean? = null
    private var fobNear: Boolean? = null
    private var pendingReply: ((Boolean, String) -> Unit)? = null

    private lateinit var stateText: TextView
    private lateinit var subText: TextView
    private lateinit var replyText: TextView
    private lateinit var pairCard: MaterialCardView
    private lateinit var keyInput: EditText
    private lateinit var connectBtn: MaterialButton

    private fun prefs() = getSharedPreferences("votol_pod", Context.MODE_PRIVATE)
    private fun key(): String? = prefs().getString("key", null)

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_main)

        stateText = findViewById(R.id.stateText)
        subText = findViewById(R.id.subText)
        replyText = findViewById(R.id.replyText)
        pairCard = findViewById(R.id.pairCard)
        keyInput = findViewById(R.id.keyInput)
        connectBtn = findViewById(R.id.connectBtn)

        adapter = (getSystemService(BLUETOOTH_SERVICE) as BluetoothManager).adapter

        findViewById<MaterialButton>(R.id.saveKeyBtn).setOnClickListener { saveKey() }
        connectBtn.setOnClickListener { connectOrDisconnect() }
        findViewById<MaterialButton>(R.id.disarmBtn).setOnClickListener { send("DISARM") }
        findViewById<MaterialButton>(R.id.armBtn).setOnClickListener { send("ARM") }
        findViewById<MaterialButton>(R.id.panicBtn).setOnClickListener { send("PANIC") }

        refreshUi()
        ensurePermissions()
    }

    /* ----------------------------- pairing key ----------------------------- */

    private fun parseKey(raw: String): String? {
        val t = raw.trim().uppercase()
        val m = Regex("^VOTOL:([0-9A-F]{32})$").find(t)
        if (m != null) return m.groupValues[1]
        return if (Regex("^[0-9A-F]{32}$").matches(t)) t else null
    }

    private fun saveKey() {
        val k = parseKey(keyInput.text.toString())
        if (k == null) {
            toast("not a VOTOL key (VOTOL:<32 hex> or 32 hex)")
            return
        }
        prefs().edit().putString("key", k).apply()
        toast("key saved")
        refreshUi()
    }

    /* ------------------------------ permissions ---------------------------- */

    private fun ensurePermissions() {
        val need = if (Build.VERSION.SDK_INT >= 31) {
            arrayOf(Manifest.permission.BLUETOOTH_SCAN, Manifest.permission.BLUETOOTH_CONNECT)
        } else {
            arrayOf(Manifest.permission.ACCESS_FINE_LOCATION)
        }
        val missing = need.filter {
            ContextCompat.checkSelfPermission(this, it) != PackageManager.PERMISSION_GRANTED
        }
        if (missing.isNotEmpty()) ActivityCompat.requestPermissions(this, missing.toTypedArray(), 1)
    }

    override fun onRequestPermissionsResult(
        requestCode: Int, permissions: Array<out String>, grantResults: IntArray,
    ) {
        super.onRequestPermissionsResult(requestCode, permissions, grantResults)
        if (requestCode != 1) return
        if (grantResults.any { it != PackageManager.PERMISSION_GRANTED }) {
            subText.text = "Bluetooth permissions denied — allow them in Settings"
        }
    }

    private fun hasBlePermission(): Boolean = if (Build.VERSION.SDK_INT >= 31) {
        ActivityCompat.checkSelfPermission(this, Manifest.permission.BLUETOOTH_SCAN) ==
            PackageManager.PERMISSION_GRANTED &&
            ActivityCompat.checkSelfPermission(this, Manifest.permission.BLUETOOTH_CONNECT) ==
            PackageManager.PERMISSION_GRANTED
    } else {
        ActivityCompat.checkSelfPermission(this, Manifest.permission.ACCESS_FINE_LOCATION) ==
            PackageManager.PERMISSION_GRANTED
    }

    /* ------------------------------- scanning ------------------------------ */

    private fun connectOrDisconnect() {
        if (gatt != null) {
            disconnect()
            return
        }
        if (key() == null) {
            toast("save the pairing key first")
            return
        }
        if (adapter == null || adapter!!.isEnabled == false) {
            subText.text = "Bluetooth is off — enable it and tap Connect"
            return
        }
        if (!hasBlePermission()) {
            ensurePermissions()
            return
        }
        startScan()
    }

    @SuppressLint("MissingPermission")
    private fun startScan() {
        if (scanning) return
        scanning = true
        refreshUi()
        subText.text = "scanning for the pod… (stand near the bike)"
        val scanner = adapter?.bluetoothLeScanner ?: run { scanning = false; return }
        val filters = listOf(
            ScanFilter.Builder().setServiceUuid(android.os.ParcelUuid(svcUuid)).build()
        )
        val settings = ScanSettings.Builder()
            .setScanMode(ScanSettings.SCAN_MODE_LOW_LATENCY)
            .build()
        scanner.startScan(filters, settings, scanCb)
        main.postDelayed({
            if (scanning) stopScan("pod not found — is it powered and awake?")
        }, 15000)
    }

    @SuppressLint("MissingPermission")
    private fun stopScan(msg: String?) {
        if (!scanning) return
        scanning = false
        try {
            adapter?.bluetoothLeScanner?.stopScan(scanCb)
        } catch (e: Exception) {
            Log.w("PodRemote", "stopScan: $e")
        }
        if (msg != null) subText.text = msg
        refreshUi()
    }

    private val scanCb = object : ScanCallback() {
        @SuppressLint("MissingPermission")
        override fun onScanResult(callbackType: Int, result: ScanResult) {
            stopScan(null)
            subText.text = "pod found — connecting…"
            result.device.connectGatt(this@MainActivity, false, gattCb)
        }

        override fun onScanFailed(errorCode: Int) {
            stopScan("scan failed ($errorCode) — check Bluetooth + location")
        }
    }

    /* --------------------------------- GATT -------------------------------- */

    private val gattCb = object : BluetoothGattCallback() {
        @SuppressLint("MissingPermission")
        override fun onConnectionStateChange(g: BluetoothGatt, status: Int, newState: Int) {
            when (newState) {
                BluetoothProfile.STATE_CONNECTED -> g.discoverServices()
                BluetoothProfile.STATE_DISCONNECTED -> runOnUiThread {
                    cleanupGatt()
                    subText.text = "disconnected"
                    refreshUi()
                }
            }
        }

        @SuppressLint("MissingPermission")
        override fun onServicesDiscovered(g: BluetoothGatt, status: Int) {
            if (status != BluetoothGatt.GATT_SUCCESS) {
                runOnUiThread { subText.text = "service discovery failed" }
                return
            }
            val svc = g.getService(svcUuid)
            if (svc == null) {
                runOnUiThread {
                    subText.text = "pod has no VOTOL service — wrong device?"
                    cleanupGatt()
                    refreshUi()
                }
                return
            }
            cmdChar = svc.getCharacteristic(cmdUuid)
            statChar = svc.getCharacteristic(statUuid)
            statChar?.let { ch ->
                g.setCharacteristicNotification(ch, true)
                val d = ch.getDescriptor(cccUuid)
                if (d != null) {
                    d.value = BluetoothGattDescriptor.ENABLE_NOTIFICATION_VALUE
                    g.writeDescriptor(d)
                }
                g.readCharacteristic(ch)   // seed the hero with current status
            }
            runOnUiThread {
                subText.text = "connected"
                refreshUi()
            }
        }

        @Deprecated("pre-33 path")
        override fun onCharacteristicRead(
            g: BluetoothGatt, ch: BluetoothGattCharacteristic, status: Int,
        ) {
            if (ch.uuid == statUuid && status == BluetoothGatt.GATT_SUCCESS) {
                ch.value?.toString(Charsets.UTF_8)?.let { handleFrame(it) }
            }
        }

        @Deprecated("pre-33 path")
        override fun onCharacteristicChanged(g: BluetoothGatt, ch: BluetoothGattCharacteristic) {
            if (ch.uuid == statUuid) {
                ch.value?.toString(Charsets.UTF_8)?.let { handleFrame(it) }
            }
        }
    }

    private fun cleanupGatt() {
        pendingReply?.invoke(false, "connection lost")
        pendingReply = null
        try {
            gatt?.close()
        } catch (_: Exception) {}
        gatt = null
        cmdChar = null
        statChar = null
    }

    private fun disconnect() {
        try {
            gatt?.disconnect()
        } catch (_: Exception) {}
        cleanupGatt()
        subText.text = "disconnected"
        refreshUi()
    }

    /* ------------------------------- protocol ------------------------------ */

    private fun handleFrame(text: String) {
        val frame = text.trim()
        runOnUiThread {
            if (frame.startsWith("OK") || frame.startsWith("ERR")) {
                replyText.text = frame
                pendingReply?.invoke(frame.startsWith("OK"), frame)
                pendingReply = null
                val m = Regex("\\b(ARMED|DISARMED|ARM|DISARM)\\b").find(frame)
                if (m != null) armed = m.groupValues[1].startsWith("ARM")
            } else {
                val m = Regex("^(ARMED|DISARMED)\\s+(FON|FOFF)").find(frame)
                if (m != null) {
                    armed = m.groupValues[1] == "ARMED"
                    fobNear = m.groupValues[2] == "FON"
                }
            }
            refreshUi()
        }
    }

    @SuppressLint("MissingPermission")
    private fun send(cmd: String) {
        val g = gatt ?: run { toast("not connected"); return }
        val ch = cmdChar ?: run { toast("link not ready"); return }
        val k = key() ?: run { toast("save the pairing key first"); return }
        if (pendingReply != null) { toast("busy — wait for the reply"); return }

        replyText.text = "$cmd …"
        pendingReply = { ok, msg ->
            runOnUiThread {
                replyText.text = msg
                toast(if (ok) "$cmd ok" else "$cmd refused")
            }
        }
        main.postDelayed({
            if (pendingReply != null) {
                pendingReply = null
                replyText.text = "no reply — pod asleep or out of range"
            }
        }, 5000)

        val payload = "$cmd:$k"
        ch.value = payload.toByteArray(Charsets.UTF_8)
        if (!g.writeCharacteristic(ch)) {
            pendingReply = null
            replyText.text = "write failed"
        }
    }

    /* ---------------------------------- UI --------------------------------- */

    private fun refreshUi() {
        val connected = gatt != null
        val hasKey = key() != null
        pairCard.visibility = if (hasKey && connected) android.view.View.GONE else android.view.View.VISIBLE

        connectBtn.text = when {
            scanning -> "scanning…"
            connected -> "Disconnect"
            else -> "Connect"
        }

        val state = when {
            connected && armed == true -> "🔒 ARMED" to "#EF4444"
            connected && armed == false -> "🔓 disarmed" to "#10B981"
            connected -> "❔ unknown" to "#F5F7FB"
            else -> "offline" to "#9AA3B2"
        }
        stateText.text = state.first
        stateText.setTextColor(android.graphics.Color.parseColor(state.second))

        if (connected) {
            val fob = when (fobNear) {
                true -> "fob near"
                false -> "fob away"
                null -> "fob ?"
            }
            if (subText.text.toString() in setOf("connected", "disconnected", "not connected") ||
                subText.text.toString().startsWith("pod") || subText.text.toString().isEmpty()
            ) {
                subText.text = "connected · $fob"
            }
        }
    }

    private fun toast(msg: String) = Toast.makeText(this, msg, Toast.LENGTH_SHORT).show()
}
