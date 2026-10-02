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
import android.content.res.ColorStateList
import android.content.res.Configuration
import android.os.Build
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.util.Log
import android.view.View
import android.widget.EditText
import android.widget.ImageButton
import android.widget.ImageView
import android.widget.FrameLayout
import android.widget.TextView
import android.widget.Toast
import androidx.appcompat.app.AlertDialog
import androidx.appcompat.app.AppCompatActivity
import androidx.appcompat.app.AppCompatDelegate
import androidx.core.app.ActivityCompat
import androidx.core.content.ContextCompat
import com.google.android.material.bottomnavigation.BottomNavigationView
import com.google.android.material.materialswitch.MaterialSwitch
import com.journeyapps.barcodescanner.ScanContract
import com.journeyapps.barcodescanner.ScanOptions
import java.util.UUID

/**
 * VOTOL Pod Remote — mobile control center for the tft-dash display pod.
 *
 * Three sections with Material 3 Bottom Navigation:
 *  1. Control: Hero status, battery voltage, fob presence, ARM / DISARM toggle, PANIC.
 *  2. Config: Manage PIN, wheel circumference, physical fobs, bridge BT dial, dark theme.
 *  3. Pairing: QR scanner (SYS → SET), manual key entry, forget pod.
 *
 * v3.0: full UI/UX redesign (amber brand, state-tinted hero, arrow-chip CTA,
 * icon-badged config cards) + light/dark theme switcher (header button + Config switch).
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
    private lateinit var pageSubtitle: TextView
    private lateinit var linkBadge: TextView
    private lateinit var themeToggle: ImageButton
    private lateinit var darkThemeSwitch: MaterialSwitch
    private lateinit var bottomNav: BottomNavigationView

    // Views: Control Tab
    private lateinit var viewControl: View
    private lateinit var heroCard: com.google.android.material.card.MaterialCardView
    private lateinit var heroRings: FrameLayout
    private lateinit var heroIcon: ImageView
    private lateinit var stateText: TextView
    private lateinit var subText: TextView
    private lateinit var voltText: TextView
    private lateinit var fobText: TextView
    private lateinit var ctaBtn: View
    private lateinit var ctaIcon: ImageView
    private lateinit var ctaLabel: TextView
    private lateinit var ctaSub: TextView
    private lateinit var ctaChip: FrameLayout
    private lateinit var ctaArrow: ImageView
    private lateinit var btnDisconnect: com.google.android.material.button.MaterialButton
    private lateinit var btnPanic: com.google.android.material.button.MaterialButton
    private lateinit var replyText: TextView

    // Views: Config Tab
    private lateinit var viewConfig: View
    private lateinit var refreshCfgBtn: View
    private lateinit var cfgPinStatus: TextView
    private lateinit var newPinInput: EditText
    private lateinit var setPinBtn: com.google.android.material.button.MaterialButton
    private lateinit var cfgWheelStatus: TextView
    private lateinit var wheelInput: EditText
    private lateinit var setWheelBtn: com.google.android.material.button.MaterialButton
    private lateinit var cfgFobStatus: TextView
    private lateinit var clrFobsBtn: com.google.android.material.button.MaterialButton
    private lateinit var cfgBtStatus: TextView
    private lateinit var btOffBtn: com.google.android.material.button.MaterialButton
    private lateinit var btOnBtn: com.google.android.material.button.MaterialButton

    // Views: Pairing Tab
    private lateinit var viewPairing: View
    private lateinit var scanBtn: View
    private lateinit var keyInput: EditText
    private lateinit var saveKeyBtn: com.google.android.material.button.MaterialButton
    private lateinit var forgetKeyBtn: com.google.android.material.button.MaterialButton

    private fun prefs() = getSharedPreferences("votol_pod", Context.MODE_PRIVATE)
    private fun key(): String? = prefs().getString("key", null)

    /* ------------------------------ theme ------------------------------ */

    private fun isDark(): Boolean =
        (resources.configuration.uiMode and Configuration.UI_MODE_NIGHT_MASK) ==
            Configuration.UI_MODE_NIGHT_YES

    private fun applyThemePref() {
        val dark = prefs().getBoolean("darkTheme", true)
        AppCompatDelegate.setDefaultNightMode(
            if (dark) AppCompatDelegate.MODE_NIGHT_YES else AppCompatDelegate.MODE_NIGHT_NO
        )
    }

    private fun setDarkTheme(dark: Boolean) {
        prefs().edit().putBoolean("darkTheme", dark).apply()
        AppCompatDelegate.setDefaultNightMode(
            if (dark) AppCompatDelegate.MODE_NIGHT_YES else AppCompatDelegate.MODE_NIGHT_NO
        )
    }

    private fun refreshThemeControls() {
        // in dark mode offer the sun (tap → light), in light offer the moon
        themeToggle.setImageResource(if (isDark()) R.drawable.ic_sun else R.drawable.ic_moon)
        if (darkThemeSwitch.isChecked != isDark()) darkThemeSwitch.isChecked = isDark()
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        applyThemePref()                       // must run before the activity inflates
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_main)

        bindViews()
        setupListeners()

        adapter = (getSystemService(BLUETOOTH_SERVICE) as BluetoothManager).adapter

        // Show saved key in input if present
        key()?.let { keyInput.setText(it) }

        refreshThemeControls()
        refreshUi()
        ensurePermissions()
    }

    private fun bindViews() {
        pageTitle = findViewById(R.id.pageTitle)
        pageSubtitle = findViewById(R.id.pageSubtitle)
        linkBadge = findViewById(R.id.linkBadge)
        themeToggle = findViewById(R.id.themeToggle)
        darkThemeSwitch = findViewById(R.id.darkThemeSwitch)
        bottomNav = findViewById(R.id.bottomNav)

        viewControl = findViewById(R.id.viewControl)
        heroCard = findViewById(R.id.heroCard)
        heroRings = findViewById(R.id.heroRings)
        heroIcon = findViewById(R.id.heroIcon)
        stateText = findViewById(R.id.stateText)
        subText = findViewById(R.id.subText)
        voltText = findViewById(R.id.voltText)
        fobText = findViewById(R.id.fobText)
        ctaBtn = findViewById(R.id.ctaBtn)
        ctaIcon = findViewById(R.id.ctaIcon)
        ctaLabel = findViewById(R.id.ctaLabel)
        ctaSub = findViewById(R.id.ctaSub)
        ctaChip = findViewById(R.id.ctaChip)
        ctaArrow = findViewById(R.id.ctaArrow)
        btnDisconnect = findViewById(R.id.btnDisconnect)
        btnPanic = findViewById(R.id.btnPanic)
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

        themeToggle.setOnClickListener { setDarkTheme(!isDark()) }
        darkThemeSwitch.setOnCheckedChangeListener { _, checked -> setDarkTheme(checked) }

        // Control Tab
        btnDisconnect.setOnClickListener { connectOrDisconnect() }
        ctaBtn.setOnClickListener { handleSecurityToggle() }
        btnPanic.setOnClickListener { send("PANIC") }

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

        when (idx) {
            0 -> { pageTitle.text = "Remote Control"; pageSubtitle.text = "Arm, disarm and monitor your bike" }
            1 -> { pageTitle.text = "Configuration"; pageSubtitle.text = "Configure the display pod" }
            else -> { pageTitle.text = "Device Pairing"; pageSubtitle.text = "Link your phone with the pod" }
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
        subText.text = "searching for pod… stand near bike"
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

    private fun handleSecurityToggle() {
        if (!isConnected) {
            connectOrDisconnect()
            return
        }
        if (armed == true) {
            // Currently ARMED -> DISARM immediately
            send("DISARM")
        } else {
            // Currently DISARMED or unknown -> confirm before arming
            AlertDialog.Builder(this)
                .setTitle("ARM the alarm?")
                .setMessage("Pod screen will enter armed standby mode.")
                .setPositiveButton("ARM") { _, _ -> send("ARM") }
                .setNegativeButton("Cancel", null)
                .show()
        }
    }

    private fun updateConfigUi() {
        cfgPinStatus.text = "Current PIN: $podPin"
        cfgWheelStatus.text = "Wheel circumference: $podWheel m"
        cfgFobStatus.text = "Registered fobs: $podFobCount"
        cfgBtStatus.text = "Bridge BT dial: ${if (podBtOn) "ON (Bluetooth)" else "OFF (CAN Bus mode)"}"
    }

    private fun col(res: Int) = ContextCompat.getColor(this, res)
    private fun tint(v: ImageView, res: Int) { v.imageTintList = ColorStateList.valueOf(col(res)) }

    private fun refreshUi() {
        runOnUiThread {
            refreshThemeControls()

            // Header link pill
            when {
                isConnected -> {
                    linkBadge.text = "LINKED"
                    linkBadge.setBackgroundResource(R.drawable.bg_pill_good)
                    linkBadge.setTextColor(col(R.color.stateGreen))
                }
                scanning -> {
                    linkBadge.text = "SEARCHING"
                    linkBadge.setBackgroundResource(R.drawable.bg_pill_warn)
                    linkBadge.setTextColor(col(R.color.stateWarn))
                }
                else -> {
                    linkBadge.text = "OFFLINE"
                    linkBadge.setBackgroundResource(R.drawable.bg_pill_gray)
                    linkBadge.setTextColor(col(R.color.statGray))
                }
            }

            // Secondary connect/disconnect tile
            btnDisconnect.text = when {
                scanning -> "Scanning…"
                isConnected -> "Disconnect"
                gatt != null -> "Connecting…"
                else -> "Connect"
            }

            // Hero state + primary CTA
            when {
                !isConnected -> {
                    heroRings.setBackgroundResource(R.drawable.rings_offline)
                    heroIcon.setImageResource(R.drawable.ic_link_off)
                    tint(heroIcon, R.color.statGray)
                    stateText.text = "OFFLINE"
                    stateText.setTextColor(col(R.color.statGray))
                    heroCard.setCardBackgroundColor(col(R.color.heroOfflineBg))
                    if (!scanning && gatt == null) subText.text = "not connected"
                    voltText.text = "--.- V"
                    fobText.text = "--"

                    ctaBtn.setBackgroundResource(R.drawable.bg_cta_connect)
                    ctaIcon.setImageResource(R.drawable.ic_radar)
                    tint(ctaIcon, R.color.textSecondary)
                    ctaLabel.text = "Connect to Control"
                    ctaLabel.setTextColor(col(R.color.textPrimary))
                    ctaSub.text = "link with the display pod"
                    ctaChip.setBackgroundResource(R.drawable.bg_chip)
                    tint(ctaArrow, R.color.textSecondary)
                }
                armed == true -> {
                    heroRings.setBackgroundResource(R.drawable.rings_armed)
                    heroIcon.setImageResource(R.drawable.ic_no_entry)
                    tint(heroIcon, R.color.stateRed)
                    stateText.text = "ARMED"
                    stateText.setTextColor(col(R.color.stateRed))
                    heroCard.setCardBackgroundColor(col(R.color.heroArmedBg))
                    subText.text = "alarm active — bike is locked"
                    voltText.text = voltage
                    fobText.text = if (fobNear == true) "near" else "away"
                    fobText.setTextColor(
                        col(if (fobNear == true) R.color.stateGreen else R.color.stateRed))

                    ctaBtn.setBackgroundResource(R.drawable.bg_cta_disarm)
                    ctaIcon.setImageResource(R.drawable.ic_lock_open)
                    tint(ctaIcon, R.color.onGreen)
                    ctaLabel.text = "DISARM"
                    ctaLabel.setTextColor(col(R.color.onGreen))
                    ctaSub.text = "tap to unlock the bike"
                    ctaChip.setBackgroundResource(R.drawable.bg_chip_on_accent)
                    tint(ctaArrow, R.color.onGreen)
                }
                armed == false -> {
                    heroRings.setBackgroundResource(R.drawable.rings_disarmed)
                    heroIcon.setImageResource(R.drawable.ic_lock_open)
                    tint(heroIcon, R.color.stateGreen)
                    stateText.text = "DISARMED"
                    stateText.setTextColor(col(R.color.stateGreen))
                    heroCard.setCardBackgroundColor(col(R.color.heroDisarmedBg))
                    subText.text = "keyless active — ready to ride"
                    voltText.text = voltage
                    fobText.text = if (fobNear == true) "near" else "away"
                    fobText.setTextColor(
                        col(if (fobNear == true) R.color.stateGreen else R.color.statGray))

                    ctaBtn.setBackgroundResource(R.drawable.bg_cta_arm)
                    ctaIcon.setImageResource(R.drawable.ic_lock)
                    tint(ctaIcon, R.color.stateRed)
                    ctaLabel.text = "ARM"
                    ctaLabel.setTextColor(col(R.color.stateRed))
                    ctaSub.text = "tap to lock & arm the bike"
                    ctaChip.setBackgroundResource(R.drawable.bg_chip)
                    tint(ctaArrow, R.color.stateRed)
                }
                else -> {
                    // linked but no status frame yet
                    heroRings.setBackgroundResource(R.drawable.rings_offline)
                    heroIcon.setImageResource(R.drawable.ic_help)
                    tint(heroIcon, R.color.textSecondary)
                    stateText.text = "LINKED"
                    stateText.setTextColor(col(R.color.textPrimary))
                    heroCard.setCardBackgroundColor(col(R.color.heroOfflineBg))
                    subText.text = "waiting for pod status…"
                    voltText.text = voltage
                    fobText.text = "--"

                    ctaBtn.setBackgroundResource(R.drawable.bg_cta_connect)
                    ctaIcon.setImageResource(R.drawable.ic_lock)
                    tint(ctaIcon, R.color.textSecondary)
                    ctaLabel.text = "ARM"
                    ctaLabel.setTextColor(col(R.color.textPrimary))
                    ctaSub.text = "waiting for status…"
                    ctaChip.setBackgroundResource(R.drawable.bg_chip)
                    tint(ctaArrow, R.color.textSecondary)
                }
            }
        }
    }

    private fun toast(msg: String) = Toast.makeText(this, msg, Toast.LENGTH_SHORT).show()
}
