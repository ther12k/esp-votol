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
import android.graphics.Color
import android.os.Build
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.util.Log
import android.view.View
import android.widget.EditText
import android.widget.TextView
import android.widget.Toast
import androidx.appcompat.app.AlertDialog
import androidx.appcompat.app.AppCompatActivity
import androidx.core.app.ActivityCompat
import androidx.core.content.ContextCompat
import com.google.android.material.bottomnavigation.BottomNavigationView
import com.google.android.material.button.MaterialButton
import com.google.android.material.card.MaterialCardView
import com.journeyapps.barcodescanner.ScanContract
import com.journeyapps.barcodescanner.ScanOptions
import java.util.UUID

/**
 * VOTOL Pod Remote — mobile control center for the tft-dash display pod.
 *
 * Three sections with Material 3 Bottom Navigation:
 *  1. Control: Hero status, battery voltage, fob presence, DISARM / ARM / PANIC.
 *  2. Config: Manage PIN, wheel circumference, physical fobs, bridge BT dial.
 *  3. Pairing: QR scanner (SYS → SET), manual key entry, forget pod.
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
    private var isConnected = false
    private var scanning = false

    // State
    private var armed: Boolean? = null
    private var fobNear: Boolean? = null
    private var voltage: String = "--.- V"
    private var pendingReply: ((Boolean, String) -> Unit)? = null

    // Config cache from pod
    private var podPin: String = "--"
    private var podWheel: String = "--"
    private var podFobCount: Int = 0
    private var podBtOn: Boolean = false

    // Views: Global
    private lateinit var pageTitle: TextView
    private lateinit var linkBadge: TextView
    private lateinit var bottomNav: BottomNavigationView

    // Views: Control Tab
    private lateinit var viewControl: View
    private lateinit var statusCard: MaterialCardView
    private lateinit var stateIcon: TextView
    private lateinit var stateText: TextView
    private lateinit var subText: TextView
    private lateinit var voltText: TextView
    private lateinit var fobText: TextView
    private lateinit var connectBtn: MaterialButton
    private lateinit var disarmBtn: MaterialButton
    private lateinit var armBtn: MaterialButton
    private lateinit var panicBtn: MaterialButton
    private lateinit var replyText: TextView

    // Views: Config Tab
    private lateinit var viewConfig: View
    private lateinit var refreshCfgBtn: MaterialButton
    private lateinit var cfgPinStatus: TextView
    private lateinit var newPinInput: EditText
    private lateinit var setPinBtn: MaterialButton
    private lateinit var cfgWheelStatus: TextView
    private lateinit var wheelInput: EditText
    private lateinit var setWheelBtn: MaterialButton
    private lateinit var cfgFobStatus: TextView
    private lateinit var clrFobsBtn: MaterialButton
    private lateinit var cfgBtStatus: TextView
    private lateinit var btOffBtn: MaterialButton
    private lateinit var btOnBtn: MaterialButton

    // Views: Pairing Tab
    private lateinit var viewPairing: View
    private lateinit var scanBtn: MaterialButton
    private lateinit var keyInput: EditText
    private lateinit var saveKeyBtn: MaterialButton
    private lateinit var forgetKeyBtn: MaterialButton

    private fun prefs() = getSharedPreferences("votol_pod", Context.MODE_PRIVATE)
    private fun key(): String? = prefs().getString("key", null)

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_main)

        bindViews()
        setupListeners()

        adapter = (getSystemService(BLUETOOTH_SERVICE) as BluetoothManager).adapter

        // Show saved key in input if present
        key()?.let { keyInput.setText(it) }

        refreshUi()
        ensurePermissions()
    }

    private fun bindViews() {
        pageTitle = findViewById(R.id.pageTitle)
        linkBadge = findViewById(R.id.linkBadge)
        bottomNav = findViewById(R.id.bottomNav)

        viewControl = findViewById(R.id.viewControl)
        statusCard = findViewById(R.id.statusCard)
        stateIcon = findViewById(R.id.stateIcon)
        stateText = findViewById(R.id.stateText)
        subText = findViewById(R.id.subText)
        voltText = findViewById(R.id.voltText)
        fobText = findViewById(R.id.fobText)
        connectBtn = findViewById(R.id.connectBtn)
        disarmBtn = findViewById(R.id.disarmBtn)
        armBtn = findViewById(R.id.armBtn)
        panicBtn = findViewById(R.id.panicBtn)
        replyText = findViewById(R.id.replyText)

        viewConfig = findViewById(R.id.viewConfig)
        refreshCfgBtn = findViewById(R.id.refreshCfgBtn)
        cfgPinStatus = findViewById(R.id.cfgPinStatus)
        newPinInput = findViewById(R.id.newPinInput)
        setPinBtn = findViewById(R.id.setPinBtn)
        cfgWheelStatus = findViewById(R.id.cfgWheelStatus)
        wheelInput = findViewById(R.id.wheelInput)
        setWheelBtn = findViewById(R.id.setWheelBtn)
        cfgFobStatus = findViewById(R.id.cfgFobStatus)
        clrFobsBtn = findViewById(R.id.clrFobsBtn)
        cfgBtStatus = findViewById(R.id.cfgBtStatus)
        btOffBtn = findViewById(R.id.btOffBtn)
        btOnBtn = findViewById(R.id.btOnBtn)

        viewPairing = findViewById(R.id.viewPairing)
        scanBtn = findViewById(R.id.scanBtn)
        keyInput = findViewById(R.id.keyInput)
        saveKeyBtn = findViewById(R.id.saveKeyBtn)
        forgetKeyBtn = findViewById(R.id.forgetKeyBtn)
    }

    private fun setupListeners() {
        bottomNav.setOnItemSelectedListener { item ->
            when (item.itemId) {
                R.id.nav_control -> switchTab(0)
                R.id.nav_config -> switchTab(1)
                R.id.nav_pairing -> switchTab(2)
            }
            true
        }

        // Control Tab
        connectBtn.setOnClickListener { connectOrDisconnect() }
        disarmBtn.setOnClickListener { send("DISARM") }
        armBtn.setOnClickListener {
            AlertDialog.Builder(this)
                .setTitle("ARM the alarm?")
                .setMessage("Pod screen will enter armed standby mode.")
                .setPositiveButton("ARM") { _, _ -> send("ARM") }
                .setNegativeButton("Cancel", null)
                .show()
        }
        panicBtn.setOnClickListener { send("PANIC") }

        // Config Tab
        refreshCfgBtn.setOnClickListener { send("GETCFG") }
        setPinBtn.setOnClickListener {
            val pin = newPinInput.text.toString().trim()
            if (pin.length in 4..12) {
                send("SETPIN", pin)
                newPinInput.setText("")
            } else {
                toast("PIN must be 4 to 12 characters")
            }
        }
        setWheelBtn.setOnClickListener {
            val w = wheelInput.text.toString().trim()
            val num = w.toFloatOrNull()
            if (num != null && num in 0.5f..5.0f) {
                send("SETWHEEL", w)
                wheelInput.setText("")
            } else {
                toast("Wheel must be between 0.5 and 5.0 metres")
            }
        }
        clrFobsBtn.setOnClickListener {
            AlertDialog.Builder(this)
                .setTitle("Clear all fobs?")
                .setMessage("All registered physical iTags will be removed from the pod.")
                .setPositiveButton("Clear All") { _, _ -> send("CLRFOB") }
                .setNegativeButton("Cancel", null)
                .show()
        }
        btOffBtn.setOnClickListener { send("SETBT", "0") }
        btOnBtn.setOnClickListener { send("SETBT", "1") }

        // Pairing Tab
        scanBtn.setOnClickListener { launchQrScan() }
        saveKeyBtn.setOnClickListener { saveKey() }
        forgetKeyBtn.setOnClickListener {
            AlertDialog.Builder(this)
                .setTitle("Forget pairing key?")
                .setMessage("You will need to scan the pod QR code again.")
                .setPositiveButton("Forget") { _, _ ->
                    prefs().edit().remove("key").apply()
                    keyInput.setText("")
                    toast("key removed")
                    refreshUi()
                }
                .setNegativeButton("Cancel", null)
                .show()
        }
    }

    private fun switchTab(idx: Int) {
        viewControl.visibility = if (idx == 0) View.VISIBLE else View.GONE
        viewConfig.visibility = if (idx == 1) View.VISIBLE else View.GONE
        viewPairing.visibility = if (idx == 2) View.VISIBLE else View.GONE

        pageTitle.text = when (idx) {
            0 -> "Remote Control"
            1 -> "Pod Settings"
            else -> "Pairing & Key"
        }

        if (idx == 1 && isConnected) {
            // Auto fetch current config on entering config tab
            send("GETCFG")
        }
    }

    /* ----------------------------- pairing key ----------------------------- */

    private fun parseKey(raw: String): String? {
        val t = raw.trim().uppercase()
        val m = Regex("^VOTOL:([0-9A-F]{32})$").find(t)
        if (m != null) return m.groupValues[1]
        if (Regex("^[0-9A-F]{32}$").matches(t)) return t
        if (Regex("^[0-9A-Z]{4,12}$").matches(t)) return t
        return null
    }

    private val scanLauncher = registerForActivityResult(ScanContract()) { result ->
        val contents = result.contents
        if (contents.isNullOrBlank()) return@registerForActivityResult
        keyInput.setText(contents)
        saveKey()
    }

    private fun launchQrScan() {
        scanLauncher.launch(
            ScanOptions()
                .setDesiredBarcodeFormats(ScanOptions.QR_CODE)
                .setPrompt("Point at the pod QR: SYS → SET")
                .setBeepEnabled(false)
                .setOrientationLocked(true)
        )
    }

    private fun saveKey() {
        val k = parseKey(keyInput.text.toString())
        if (k == null) {
            toast("Invalid key format (must be 32-hex or 4-12 PIN)")
            return
        }
        prefs().edit().putString("key", k).apply()
        toast("Key saved successfully!")
        refreshUi()
        switchTab(0)
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
        if (isConnected || gatt != null) {
            disconnect()
            return
        }
        if (key() == null) {
            toast("Pair with the pod first (Scan QR)")
            switchTab(2)
            return
        }
        if (adapter?.isEnabled != true) {
            subText.text = "Bluetooth is disabled on phone"
            toast("Please enable Bluetooth")
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
        subText.text = "scanning for pod… stand near bike"
        refreshUi()

        val scanner = adapter?.bluetoothLeScanner ?: run { scanning = false; return }
        val filters = listOf(
            ScanFilter.Builder().setServiceUuid(android.os.ParcelUuid(svcUuid)).build(),
            ScanFilter.Builder().setDeviceName("votol-dash").build()
        )
        val settings = ScanSettings.Builder()
            .setScanMode(ScanSettings.SCAN_MODE_LOW_LATENCY)
            .build()

        scanner.startScan(filters, settings, scanCb)
        main.postDelayed({
            if (scanning) stopScan("pod not found — is it powered on?")
        }, 12000)
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
            val devName = try { result.device.name } catch (_: Exception) { null }
                ?: result.scanRecord?.deviceName
            val hasUuid = result.scanRecord?.serviceUuids?.any { it.uuid == svcUuid } == true
            if (devName == "votol-dash" || hasUuid || devName?.contains("votol", ignoreCase = true) == true) {
                stopScan(null)
                subText.text = "pod found — connecting…"
                refreshUi()
                gatt = if (Build.VERSION.SDK_INT >= 23) {
                    result.device.connectGatt(this@MainActivity, false, gattCb, BluetoothDevice.TRANSPORT_LE)
                } else {
                    result.device.connectGatt(this@MainActivity, false, gattCb)
                }
            }
        }

        override fun onScanFailed(errorCode: Int) {
            stopScan("scan failed ($errorCode)")
        }
    }

    /* --------------------------------- GATT -------------------------------- */

    private val gattCb = object : BluetoothGattCallback() {
        @SuppressLint("MissingPermission")
        override fun onConnectionStateChange(g: BluetoothGatt, status: Int, newState: Int) {
            when (newState) {
                BluetoothProfile.STATE_CONNECTED -> {
                    main.postDelayed({
                        try { g.discoverServices() } catch (_: Exception) {}
                    }, 300)
                    runOnUiThread {
                        subText.text = "connected — discovering services…"
                        refreshUi()
                    }
                }
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
                    subText.text = "VOTOL service not found on device"
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
                    if (Build.VERSION.SDK_INT >= 33) {
                        g.writeDescriptor(d, BluetoothGattDescriptor.ENABLE_NOTIFICATION_VALUE)
                    } else {
                        d.value = BluetoothGattDescriptor.ENABLE_NOTIFICATION_VALUE
                        g.writeDescriptor(d)
                    }
                } else {
                    g.readCharacteristic(ch)
                }
            }
            isConnected = true
            runOnUiThread {
                subText.text = "linked"
                refreshUi()
            }
        }

        override fun onDescriptorWrite(g: BluetoothGatt, d: BluetoothGattDescriptor, status: Int) {
            statChar?.let { g.readCharacteristic(it) }
        }

        @Deprecated("pre-33 path")
        override fun onCharacteristicRead(
            g: BluetoothGatt, ch: BluetoothGattCharacteristic, status: Int,
        ) {
            if (ch.uuid == statUuid && status == BluetoothGatt.GATT_SUCCESS) {
                ch.value?.toString(Charsets.UTF_8)?.let { handleFrame(it) }
            }
        }

        override fun onCharacteristicRead(
            g: BluetoothGatt, ch: BluetoothGattCharacteristic, value: ByteArray, status: Int,
        ) {
            if (ch.uuid == statUuid && status == BluetoothGatt.GATT_SUCCESS) {
                value.toString(Charsets.UTF_8).let { handleFrame(it) }
            }
        }

        @Deprecated("pre-33 path")
        override fun onCharacteristicChanged(g: BluetoothGatt, ch: BluetoothGattCharacteristic) {
            if (ch.uuid == statUuid) {
                ch.value?.toString(Charsets.UTF_8)?.let { handleFrame(it) }
            }
        }

        override fun onCharacteristicChanged(
            g: BluetoothGatt, ch: BluetoothGattCharacteristic, value: ByteArray,
        ) {
            if (ch.uuid == statUuid) {
                value.toString(Charsets.UTF_8).let { handleFrame(it) }
            }
        }
    }

    private fun cleanupGatt() {
        isConnected = false
        pendingReply?.invoke(false, "connection closed")
        pendingReply = null
        try {
            gatt?.close()
        } catch (_: Exception) {}
        gatt = null
        cmdChar = null
        statChar = null
        armed = null
        fobNear = null
    }

    private fun disconnect() {
        try {
            gatt?.disconnect()
        } catch (_: Exception) {}
        subText.text = "disconnecting…"
        refreshUi()
    }

    /* ------------------------------- protocol ------------------------------ */

    private fun handleFrame(text: String) {
        val frame = text.trim()
        Log.i("PodRemote", "RX: $frame")
        runOnUiThread {
            if (frame.startsWith("OK") || frame.startsWith("ERR")) {
                replyText.text = frame
                pendingReply?.invoke(frame.startsWith("OK"), frame)
                pendingReply = null

                if (frame.startsWith("OK ARMED")) armed = true
                if (frame.startsWith("OK DISARM")) armed = false
            } else if (frame.startsWith("CFG:")) {
                // CFG:pin:wheel:fobCount:btLinkOn
                val parts = frame.split(":")
                if (parts.size >= 5) {
                    podPin = parts[1]
                    podWheel = parts[2]
                    podFobCount = parts[3].toIntOrNull() ?: 0
                    podBtOn = parts[4] == "1"
                    updateConfigUi()
                    toast("Config loaded")
                }
            } else {
                // status frame: "ARMED FON 78.5V"
                val parts = frame.split(" ")
                if (parts.isNotEmpty()) {
                    armed = parts[0] == "ARMED"
                }
                if (parts.size >= 2) {
                    fobNear = parts[1] == "FON"
                }
                if (parts.size >= 3 && parts[2].endsWith("V")) {
                    voltage = parts[2]
                }
            }
            refreshUi()
        }
    }

    @SuppressLint("MissingPermission")
    private fun send(cmd: String, arg: String = "") {
        val g = gatt ?: run { toast("Not connected"); return }
        val ch = cmdChar ?: run { toast("Link not ready"); return }
        val k = key() ?: run { toast("Save pairing key first"); return }
        if (pendingReply != null) { toast("Busy…"); return }

        replyText.text = "→ $cmd"
        pendingReply = { ok, msg ->
            runOnUiThread {
                replyText.text = msg
                toast(if (ok) "$cmd ✓" else "$cmd Refused")
            }
        }
        main.postDelayed({
            if (pendingReply != null) {
                pendingReply = null
                replyText.text = "timeout — pod didn't respond"
            }
        }, 5000)

        val payload = if (arg.isEmpty()) "$cmd:$k" else "$cmd:$k:$arg"
        val bytes = payload.toByteArray(Charsets.UTF_8)
        val writeOk = if (Build.VERSION.SDK_INT >= 33) {
            g.writeCharacteristic(ch, bytes, BluetoothGattCharacteristic.WRITE_TYPE_DEFAULT) == 0
        } else {
            ch.value = bytes
            ch.writeType = BluetoothGattCharacteristic.WRITE_TYPE_DEFAULT
            g.writeCharacteristic(ch)
        }
        if (!writeOk) {
            pendingReply = null
            replyText.text = "write failed"
        }
    }

    /* ---------------------------------- UI --------------------------------- */

    private fun updateConfigUi() {
        cfgPinStatus.text = "Current PIN: $podPin"
        cfgWheelStatus.text = "Wheel circumference: $podWheel m"
        cfgFobStatus.text = "Registered iTag fobs: $podFobCount"
        cfgBtStatus.text = "Bridge BT dial: ${if (podBtOn) "ON (Bluetooth)" else "OFF (CAN Bus mode)"}"
    }

    private fun refreshUi() {
        runOnUiThread {
            // Header badge
            if (isConnected) {
                linkBadge.text = "LINKED"
                linkBadge.setTextColor(Color.parseColor("#10B981"))
            } else if (scanning) {
                linkBadge.text = "SEARCHING"
                linkBadge.setTextColor(Color.parseColor("#F59E0B"))
            } else {
                linkBadge.text = "OFFLINE"
                linkBadge.setTextColor(Color.parseColor("#9AA3B2"))
            }

            // Connect button state
            connectBtn.text = when {
                scanning -> "Scanning…"
                isConnected -> "Disconnect"
                gatt != null -> "Connecting…"
                else -> "Connect"
            }

            // Hero state display
            when {
                !isConnected -> {
                    stateIcon.text = "📡"
                    stateText.text = "OFFLINE"
                    stateText.setTextColor(Color.parseColor("#9AA3B2"))
                    statusCard.setCardBackgroundColor(Color.parseColor("#161B27"))
                    voltText.text = "--.- V"
                    fobText.text = "--"
                }
                armed == true -> {
                    stateIcon.text = "🚫"
                    stateText.text = "ARMED"
                    stateText.setTextColor(Color.parseColor("#EF4444"))
                    statusCard.setCardBackgroundColor(Color.parseColor("#2A1115"))
                    voltText.text = voltage
                    fobText.text = if (fobNear == true) "near" else "away"
                    fobText.setTextColor(if (fobNear == true) Color.parseColor("#10B981") else Color.parseColor("#EF4444"))
                }
                armed == false -> {
                    stateIcon.text = "🔓"
                    stateText.text = "DISARMED"
                    stateText.setTextColor(Color.parseColor("#10B981"))
                    statusCard.setCardBackgroundColor(Color.parseColor("#10241C"))
                    voltText.text = voltage
                    fobText.text = if (fobNear == true) "near" else "away"
                    fobText.setTextColor(if (fobNear == true) Color.parseColor("#10B981") else Color.parseColor("#9AA3B2"))
                }
                else -> {
                    stateIcon.text = "❔"
                    stateText.text = "LINKED"
                    stateText.setTextColor(Color.parseColor("#F5F7FB"))
                    statusCard.setCardBackgroundColor(Color.parseColor("#161B27"))
                    voltText.text = voltage
                    fobText.text = "--"
                }
            }
        }
    }

    private fun toast(msg: String) = Toast.makeText(this, msg, Toast.LENGTH_SHORT).show()
}
