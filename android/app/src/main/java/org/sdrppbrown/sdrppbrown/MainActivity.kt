package org.sdrppbrown.sdrppbrown

import android.Manifest
import android.annotation.SuppressLint
import android.app.NativeActivity
import android.app.PendingIntent
import android.app.PendingIntent.FLAG_MUTABLE
import android.bluetooth.BluetoothSocket.TYPE_SCO
import android.content.ActivityNotFoundException
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.content.pm.ActivityInfo
import android.content.pm.PackageManager
import android.content.res.AssetManager
import android.hardware.Sensor
import android.hardware.SensorEvent
import android.hardware.SensorEventListener
import android.hardware.SensorManager
import android.hardware.usb.UsbDevice
import android.hardware.usb.UsbDeviceConnection
import android.hardware.usb.UsbManager
import android.media.AudioManager
import android.media.ToneGenerator
import android.net.Uri
import android.os.BatteryManager
import android.os.BatteryManager.BATTERY_PROPERTY_CAPACITY
import android.os.Build
import android.os.Bundle
import android.os.Environment
import android.os.VibrationEffect
import android.os.Vibrator
import android.provider.Settings
import android.util.Log
import android.view.KeyEvent
import android.view.View
import android.view.inputmethod.InputMethodManager
import androidx.core.app.ActivityCompat
import androidx.core.content.ContextCompat
import androidx.core.content.PermissionChecker
import org.xmlpull.v1.XmlPullParser
import java.io.BufferedReader
import java.io.File
import java.io.FileOutputStream
import java.io.InputStreamReader
import java.net.HttpURLConnection
import java.net.URL
import java.util.concurrent.LinkedBlockingQueue


private const val ACTION_USB_PERMISSION = "org.sdrppbrown.sdrppbrown.USB_PERMISSION";

class MainActivity : NativeActivity(), SensorEventListener {
    private lateinit var audioManager: AudioManager
    private lateinit var toneG: ToneGenerator
    private lateinit var thisCacheDir: String
    private val TAG: String = "SDR++Brown";
    public var usbManager: UsbManager? = null;
    public var SDR_device: UsbDevice? = null;
    public var SDR_conn: UsbDeviceConnection? = null;
    public var SDR_VID: Int = -1;
    public var SDR_PID: Int = -1;
    public var SDR_FD: Int = -1;
    public var density: Float = 1.0f;
    var batteryManager: BatteryManager? = null;

    // Registered once in onCreate and torn down in onDestroy. It used to unregister
    // itself after the very first permission answer, which meant the app got exactly
    // one attempt at one device per launch.
    private var usbReceiverRegistered = false;

    private val usbReceiver = object : BroadcastReceiver() {
        override fun onReceive(context: Context, intent: Intent) {
            Log.w("SDR++", "USB broadcast: " + intent.action.toString())
            val dev: UsbDevice? = intent.getParcelableExtra(UsbManager.EXTRA_DEVICE)
            when (intent.action) {
                ACTION_USB_PERMISSION -> {
                    synchronized(this@MainActivity) {
                        if (!intent.getBooleanExtra(UsbManager.EXTRA_PERMISSION_GRANTED, false)) {
                            Log.w("SDR++", "USB permission denied for $dev")
                        } else if (dev == null) {
                            Log.w("SDR++", "USB permission granted but no device in the intent")
                        } else {
                            Log.w("SDR++", "USB permission granted for $dev")
                            claimSdrDevice(dev)
                        }
                    }
                    hideSystemBars()
                }

                UsbManager.ACTION_USB_DEVICE_ATTACHED -> {
                    // Hot plug while we are already running. The launcher intent only
                    // covers the case where attaching started the app.
                    if (dev != null) { offerDevice(dev) }
                }

                UsbManager.ACTION_USB_DEVICE_DETACHED -> {
                    synchronized(this@MainActivity) {
                        if (dev != null && dev.deviceName == SDR_device?.deviceName) {
                            Log.w("SDR++", "SDR detached, releasing fd")
                            releaseSdrDevice()
                        }
                    }
                }
            }
        }
    }

    // The set of devices this build can actually open, read back from the same
    // resource Android uses to decide whether to offer us the device at all, so
    // there is only one list to keep up to date.
    private var cachedUsbIds: Set<Pair<Int, Int>>? = null

    private fun knownUsbIds(): Set<Pair<Int, Int>> {
        cachedUsbIds?.let { return it }
        val ids = HashSet<Pair<Int, Int>>()
        try {
            val parser = resources.getXml(R.xml.device_filter)
            var event = parser.eventType
            while (event != XmlPullParser.END_DOCUMENT) {
                if (event == XmlPullParser.START_TAG && parser.name == "usb-device") {
                    val vid = parseFilterInt(parser.getAttributeValue(null, "vendor-id"))
                    val pid = parseFilterInt(parser.getAttributeValue(null, "product-id"))
                    if (vid != null && pid != null) { ids.add(Pair(vid, pid)) }
                }
                event = parser.next()
            }
            parser.close()
        } catch (e: Exception) {
            Log.e(TAG, "Could not read device_filter.xml: ${e.message}")
        }
        Log.w("SDR++", "USB: know ${ids.size} device ids")
        cachedUsbIds = ids
        return ids
    }

    // Mirrors android.hardware.usb.DeviceFilter.read(): decimal unless 0x prefixed.
    private fun parseFilterInt(value: String?): Int? {
        if (value == null) { return null }
        return try {
            if (value.length > 2 && value[0] == '0' && (value[1] == 'x' || value[1] == 'X')) {
                value.substring(2).toInt(16)
            } else {
                value.toInt(10)
            }
        } catch (e: NumberFormatException) {
            Log.e(TAG, "Bad number in device_filter.xml: '$value'")
            null
        }
    }

    private fun isKnownSdr(dev: UsbDevice): Boolean {
        return knownUsbIds().contains(Pair(dev.vendorId, dev.productId))
    }

    // Takes ownership of a device we already hold permission for.
    private fun claimSdrDevice(dev: UsbDevice): Boolean {
        if (!isKnownSdr(dev)) {
            Log.w("SDR++", "Ignoring non-SDR device $dev")
            return false
        }
        if (SDR_FD != -1) {
            if (dev.deviceName == SDR_device?.deviceName) { return true }
            // Already driving another SDR. Leave it alone rather than pull the file
            // descriptor out from under libusb on the native side.
            Log.w("SDR++", "Already using ${SDR_device?.deviceName}, ignoring $dev")
            return false
        }
        val conn: UsbDeviceConnection? = usbManager?.openDevice(dev)
        if (conn == null) {
            Log.e(TAG, "openDevice failed for $dev")
            return false
        }
        SDR_device = dev
        SDR_conn = conn
        SDR_VID = dev.vendorId
        SDR_PID = dev.productId
        SDR_FD = conn.fileDescriptor
        Log.w("SDR++", "Using SDR " + dev.deviceName + " " +
            Integer.toHexString(SDR_VID) + ":" + Integer.toHexString(SDR_PID) + " fd=" + SDR_FD)
        return true
    }

    private fun releaseSdrDevice() {
        try {
            SDR_conn?.close()
        } catch (e: Exception) {
            Log.e(TAG, "Closing USB connection: ${e.message}")
        }
        SDR_conn = null
        SDR_device = null
        SDR_FD = -1
        SDR_VID = -1
        SDR_PID = -1
    }

    // Claims the device if we may, asks for it if we may not, ignores it if it
    // isn't something we can drive. Asking about hubs, docks and mice is what used
    // to burn the app's single permission request on the wrong device.
    private fun offerDevice(dev: UsbDevice) {
        if (!isKnownSdr(dev)) { return }
        if (usbManager?.hasPermission(dev) == true) {
            claimSdrDevice(dev)
        } else {
            Log.w("SDR++", "Requesting USB permission for $dev")
            usbManager?.requestPermission(dev, makeUsbPermissionIntent())
        }
    }

    // Android hands the device straight to us, already granted, when attaching it
    // is what launched the app. Dropping that intent on the floor is what forced
    // the old code down the guess-a-device path.
    private fun handleUsbIntent(intent: Intent?) {
        if (intent?.action != UsbManager.ACTION_USB_DEVICE_ATTACHED) { return }
        val dev: UsbDevice? = intent.getParcelableExtra(UsbManager.EXTRA_DEVICE)
        Log.w("SDR++", "Launched by USB attach: $dev")
        if (dev != null) { offerDevice(dev) }
    }

    val REQUIRED_PERMISSIONS = arrayOf(
        Manifest.permission.WRITE_EXTERNAL_STORAGE,
        Manifest.permission.READ_EXTERNAL_STORAGE,
        Manifest.permission.RECORD_AUDIO,
        Manifest.permission.INTERNET,
        Manifest.permission.BATTERY_STATS,
        Manifest.permission.HIGH_SAMPLING_RATE_SENSORS);
    val PERMISSION_REQUEST_CODE = 1001;

    fun checkAndAsk(permission: String) {
        if (PermissionChecker.checkSelfPermission(
                this,
                permission
            ) != PermissionChecker.PERMISSION_GRANTED
        ) {
            ActivityCompat.requestPermissions(this, arrayOf(permission), 1);
        }
    }

    public fun hideSystemBars() {
        val decorView = getWindow().getDecorView();
        val uiOptions = View.SYSTEM_UI_FLAG_HIDE_NAVIGATION or View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY;
        decorView.setSystemUiVisibility(uiOptions);
    }

    var playStarted = 0L

    var recentz = ArrayList<Float>();

    var compx : Float = 0.0f
    var compy : Float = 0.0f
    var compz : Float = 0.0f
    var accx : Float = 0.0f
    var accy : Float = 0.0f
    var accz : Float = 0.0f

    override fun onSensorChanged(event: SensorEvent?) {
        if (event?.sensor?.type == Sensor.TYPE_MAGNETIC_FIELD) {
            compx = event.values[0]
            compy = event.values[1]
            compz = event.values[2]
//            Log.w("SDR++", "Sensors: $accx $accy $accz $compx $compy $compz")
        }
        if (event?.sensor?.type == Sensor.TYPE_MAGNETIC_FIELD_UNCALIBRATED) {
            compx = event.values[0]
            compy = event.values[1]
            compz = event.values[2]
//            Log.w("SDR++", "Sensors: $accx $accy $accz $compx $compy $compz")
        }
        if (event?.sensor?.type == Sensor.TYPE_ACCELEROMETER) {
            accx = event.values[0]
            accy = event.values[1]
            accz = event.values[2]
            if (recentz.size > 0) {
                val prev = recentz[recentz.size-1]
                if (Math.abs(prev) < 0.5 ) {
                    if (accz - prev > 0.5) {
                        if (System.currentTimeMillis() > playStarted + 70) {
                            playStarted = System.currentTimeMillis()
//                            toneG.startTone(ToneGenerator.TONE_DTMF_1, 50)
                        }
                    }
                    if (accz - prev < -0.5) {
                        if (System.currentTimeMillis() > playStarted + 170) {
                            playStarted = System.currentTimeMillis()
//                            toneG.startTone(ToneGenerator.TONE_DTMF_2, 150)
                        }
                    }
                }
            }
            recentz.add(accz);
            if (recentz.size > 50) {
                recentz.removeAt(0);
            }

//            Log.w("SDR++", "Sensors: $accx $accy $accz $compx $compy $compz")


            // handle accelerometer values
        }
    }

    override fun onAccuracyChanged(sensor: Sensor?, accuracy: Int) {
    }

    private lateinit var sensorManager: SensorManager
    private var accelerometer: Sensor? = null
    private var compass: Sensor? = null

    override fun onDestroy() {
        super.onDestroy()
        sensorManager.unregisterListener(this)
        if (usbReceiverRegistered) {
            try {
                unregisterReceiver(usbReceiver)
            } catch (e: IllegalArgumentException) {
                // Already gone, nothing to do.
            }
            usbReceiverRegistered = false
        }
    }

    private fun registerUsbReceiver() {
        if (usbReceiverRegistered) { return }
        val filter = IntentFilter(ACTION_USB_PERMISSION)
        filter.addAction(UsbManager.ACTION_USB_DEVICE_ATTACHED)
        filter.addAction(UsbManager.ACTION_USB_DEVICE_DETACHED)
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
            // Required once the app targets 34; harmless before that.
            registerReceiver(usbReceiver, filter, Context.RECEIVER_NOT_EXPORTED)
        } else {
            registerReceiver(usbReceiver, filter)
        }
        usbReceiverRegistered = true
    }

    override fun onNewIntent(intent: Intent) {
        super.onNewIntent(intent)
        setIntent(intent)
        Log.w("SDR++", "onNewIntent: ${intent.action}")
        handleUsbIntent(intent)
    }

    fun proceedWithPermissions() {
        Log.w("SDR++", "PERM: Proceed with permissions...")
        if (REQUIRED_PERMISSIONS.all {
            ContextCompat.checkSelfPermission(baseContext, it) == PackageManager.PERMISSION_GRANTED
        }) {
            Log.w("SDR++", "PERM: All REQUIRED_PERMISSIONS passed..")
            this.permissionsPassed.add(PERMISSION_REQUEST_CODE);
        }
        if (!permissionsPassed.contains(PERMISSION_REQUEST_CODE)) {
            // Marked before asking, not after. REQUIRED_PERMISSIONS contains entries a
            // normal app can never hold (BATTERY_STATS is signature|privileged), so
            // waiting for them all to be granted loops the dialog forever.
            permissionsPassed.add(PERMISSION_REQUEST_CODE);
            Log.w("SDR++", "PERM: Requesting REQUIRED_PERMISSIONS..")
            ActivityCompat.requestPermissions(this, REQUIRED_PERMISSIONS, PERMISSION_REQUEST_CODE);
            return;
        }
        if (!permissionsPassed.contains(PERMISSION_REQUEST_CODE+1)) {
            permissionsPassed.add(PERMISSION_REQUEST_CODE+1)
            Log.w("SDR++", "PERM: Requesting USB permissions..")

            // Ask about every SDR that is already plugged in, not just whichever one
            // happened to come first out of an unordered HashMap.
            val devList = usbManager!!.deviceList
            Log.w("SDR++", "PERM: Dev list size: " + devList.size.toString())
            for ((name, dev) in devList) {
                Log.w("SDR++", "PERM: Dev list item: $name $dev")
                offerDevice(dev)
            }
            Log.w("SDR++", "PERM: End req all USB perm:")
        }

        if (!permissionsPassed.contains(PERMISSION_REQUEST_CODE+2)) {
            // filesystem crap
            Log.w("SDR++", "PERM: Req ext storage.. testing")
            val externalStorageDirectory = Environment.getExternalStorageDirectory()
            val testFile = File(externalStorageDirectory, "sdrppbrown.test")
            Log.w("SDR++", "PERM Trying Test file: $testFile")
            var success = false;
            try {
                testFile.delete()
                success = testFile.createNewFile()
            } catch (e: java.lang.Exception) {
                Log.w("SDR++", "PERM Test file create: Exception: ${e.message}")
                // no luck
            }
            if (success) {
                testFile.delete()
            } else {
                Log.w("SDR++", "PERM: Requesting ext storage.")
                try {
                    val intent = Intent(Settings.ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION)
                    val uri: Uri = Uri.fromParts("package", packageName, null)
                    intent.data = uri
                    startActivityForResult(intent, PERMISSION_REQUEST_CODE+2);
                    return;
                } catch (e: ActivityNotFoundException) {
                    Log.w("SDR++", "PERM: MANAGE_APP_ALL_FILES_ACCESS_PERMISSION not available, skipping");
                }
            }
            permissionsPassed.add(PERMISSION_REQUEST_CODE+2);

        }


    }

    var permissionsPassed = HashSet<Int>();

    var autoPermissionProceed = true;

    override fun onRequestPermissionsResult(
        requestCode: Int,
        permissions: Array<out String>,
        grantResults: IntArray
    ) {
        if (requestCode > PERMISSION_REQUEST_CODE && requestCode < PERMISSION_REQUEST_CODE + 10) {
            Log.w("SDR++", "PERM: got perm response: $requestCode")
            this.permissionsPassed.add(requestCode);
        }
        super.onRequestPermissionsResult(requestCode, permissions, grantResults)
        if (autoPermissionProceed) {
            Log.w("SDR++", "PERM: auto proceed")
            proceedWithPermissions();
        }
    }

    public override fun onCreate(savedInstanceState: Bundle?) {
        // Hide bars
        hideSystemBars();

        // temporary directory for C++
        val cacheDir = cacheDir
        this.thisCacheDir = cacheDir.absolutePath

        sensorManager = getSystemService(Context.SENSOR_SERVICE) as SensorManager
        accelerometer = sensorManager.getDefaultSensor(Sensor.TYPE_ACCELEROMETER)
        accelerometer?.let { sensorManager.registerListener(this, it, SensorManager.SENSOR_DELAY_FASTEST) }

        compass = sensorManager.getDefaultSensor(Sensor.TYPE_MAGNETIC_FIELD_UNCALIBRATED)
        if (compass != null) {
            sensorManager.registerListener(this, compass, SensorManager.SENSOR_DELAY_FASTEST)
        } else {
            compass = sensorManager.getDefaultSensor(Sensor.TYPE_MAGNETIC_FIELD)
            if (compass != null) {
                sensorManager.registerListener(this, compass, SensorManager.SENSOR_DELAY_FASTEST)
            }
        }
        compass?.let { sensorManager.registerListener(this, it, SensorManager.SENSOR_DELAY_FASTEST) }
        compass = sensorManager.getDefaultSensor(Sensor.TYPE_MAGNETIC_FIELD_UNCALIBRATED)
        compass?.let { sensorManager.registerListener(this, it, SensorManager.SENSOR_DELAY_FASTEST) }

        toneG = ToneGenerator(AudioManager.STREAM_ACCESSIBILITY, 100)




        val dm = resources.displayMetrics
        density = dm.density;
        Log.w("SDR++", "Display density: ${dm.density} scaled: ${dm.scaledDensity} densityDpi: ${dm.densityDpi} width: ${dm.widthPixels} height: ${dm.heightPixels} xdpi: ${dm.xdpi} ydpi: ${dm.ydpi} widthDp: ${dm.widthPixels/dm.density} heightDp: ${dm.heightPixels/dm.density}")

        requestedOrientation = ActivityInfo.SCREEN_ORIENTATION_USER_LANDSCAPE;

        //// cont here



        super.onCreate(savedInstanceState)


        registerReceiver(this.batteryBroadcastReceiver, IntentFilter(Intent.ACTION_BATTERY_CHANGED))

        batteryManager = getSystemService(BATTERY_SERVICE) as BatteryManager

        scanAudioDevices()

        usbManager = getSystemService(USB_SERVICE) as UsbManager
        registerUsbReceiver()

        // If attaching the SDR is what started us, Android has already granted us
        // that device and passed it in. Take it before falling back to asking.
        handleUsbIntent(getIntent())

        proceedWithPermissions();
    }

    fun scanAudioDevices() : String {
        audioManager = getSystemService(AUDIO_SERVICE) as AudioManager
        val adi = audioManager.getDevices(AudioManager.GET_DEVICES_ALL)
        _audioSinkIds = "";
        _audioSourceIds = "";
        adi.forEach {
            if (it.isSink)
                _audioSinkIds += "${it.id}\t${it.type}\t${it.productName}\t"
            if (it.isSource)
                _audioSourceIds += "${it.id}\t${it.type}\t${it.productName}\t"
        }
        if (_audioSinkIds.isNotEmpty()) _audioSinkIds =
            _audioSinkIds.substring(0, _audioSinkIds.length - 1)
        if (_audioSourceIds.isNotEmpty()) _audioSourceIds =
            _audioSourceIds.substring(0, _audioSourceIds.length - 1)
        return "OK";
    }

    var _audioSinkIds = "";
    var _audioSourceIds = "";

    fun getAudioSinkIds() = _audioSinkIds;
    fun getAudioSourceIds() = _audioSourceIds;

    fun startBtSco() : String {
        audioManager.isBluetoothScoOn = true
        audioManager.startBluetoothSco()
        audioManager.isMicrophoneMute = false;
        audioManager.mode = AudioManager.MODE_IN_COMMUNICATION
        return "OK";
    }

    fun stopBtSco() : String {
        audioManager.isBluetoothScoOn = false
        audioManager.stopBluetoothSco()
        audioManager.mode = AudioManager.MODE_NORMAL;
        return "OK";
    }

    var batteryStatusStr: String = "?";

    private val batteryBroadcastReceiver: BroadcastReceiver = object : BroadcastReceiver() {
        @SuppressLint("SetTextI18n")
        override fun onReceive(context: Context?, batteryStatus: Intent) {
            val level = batteryStatus?.getIntExtra(BatteryManager.EXTRA_LEVEL, -1) ?: -1
            val scale = batteryStatus?.getIntExtra(BatteryManager.EXTRA_SCALE, -1) ?: -1

            // Handle cases when valid level isn't available
            if (level == -1 || scale == -1) {
                batteryStatusStr = "??"
                return
            }

            val batteryPercentage = level / scale.toFloat() * 100.0f
            batteryStatusStr = String.format("%d", batteryPercentage.toInt())
        }
    }

    public fun httpGet(url: String): String {
        var response = ""
        var connection : HttpURLConnection? = null
        try {
            connection = URL(url).openConnection() as HttpURLConnection
            connection.requestMethod = "GET"
            val responseCode = connection.responseCode
            if (responseCode == HttpURLConnection.HTTP_OK) {
                val reader = BufferedReader(InputStreamReader(connection.inputStream))
                response = reader.readText()
                reader.close()
            } else {
                throw Exception("HTTP error code: $responseCode")
            }
        } catch (e: RuntimeException) {
            response = "#ERROR: + "+e.toString();
        } finally {
            connection?.disconnect()
        }
        return response
    }

    private fun makeUsbPermissionIntent(): PendingIntent {
        // setPackage makes the intent explicit. Android 14 refuses to create a mutable
        // PendingIntent around an implicit one, and the system needs it mutable so it
        // can attach EXTRA_DEVICE to the broadcast.
        val intent = Intent(ACTION_USB_PERMISSION).setPackage(packageName)
        val flags = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) FLAG_MUTABLE else 0
        return PendingIntent.getBroadcast(
            this@MainActivity,
            PERMISSION_REQUEST_CODE+1,
            intent,
            flags
        )
    }

    fun getThisCacheDir(): String {
        return thisCacheDir;
    }

    fun getDisplayDensityStr(): String {
        return this.density.toString();
    }


    public override fun onResume() {
        // Hide bars again
        hideSystemBars();
        super.onResume();
    }

    fun showSoftInput() {
        val inputMethodManager = getSystemService(INPUT_METHOD_SERVICE) as InputMethodManager;
        inputMethodManager.showSoftInput(window.decorView, 0);
    }

    fun hideSoftInput() {
        val inputMethodManager = getSystemService(INPUT_METHOD_SERVICE) as InputMethodManager;
        inputMethodManager.hideSoftInputFromWindow(window.decorView.windowToken, 0);
        hideSystemBars();
    }

    // Queue for the Unicode characters to be polled from native code (via pollUnicodeChar())
    private var unicodeCharacterQueue: LinkedBlockingQueue<Int> = LinkedBlockingQueue()

    // We assume dispatchKeyEvent() of the NativeActivity is actually called for every
    // KeyEvent and not consumed by any View before it reaches here
    override fun dispatchKeyEvent(event: KeyEvent): Boolean {
        if (event.action == KeyEvent.ACTION_DOWN) {
            unicodeCharacterQueue.offer(event.getUnicodeChar(event.metaState))
        }
        return super.dispatchKeyEvent(event)
    }

    fun pollUnicodeChar(): Int {
        return unicodeCharacterQueue.poll() ?: 0
    }

    public fun createIfDoesntExist(path: String) {
        // This is a directory, create it in the filesystem
        var folder = File(path);
        var success = true;
        if (!folder.exists()) {
            success = folder.mkdirs();
        }
        if (!success) {
            Log.e(TAG, "Could not create folder with path " + path);
        }
    }

    public fun extractDir(aman: AssetManager, local: String, rsrc: String): Int {
        val flist = aman.list(rsrc);
        var ecount = 0;
        if (flist != null) {
            for (fp in flist) {
                val lpath = local + "/" + fp;
                val rpath = rsrc + "/" + fp;

                Log.w(TAG, "Extracting '" + rpath + "' to '" + lpath + "'");

                // Create local path if non-existent
                createIfDoesntExist(local);

                // Create if directory
                val ext = extractDir(aman, lpath, rpath);

                // Extract if file
                if (ext == 0) {
                    // This is a file, extract it
                    val _os = FileOutputStream(lpath);
                    val _is = aman.open(rpath);
                    val ilen = _is.available();
                    var fbuf = ByteArray(ilen);
                    _is.read(fbuf, 0, ilen);
                    _os.write(fbuf);
                    _os.close();
                    _is.close();
                }

                ecount++;
            }
        }
        return ecount;
    }

    fun getArchitectureString(): String {
        return System.getProperty("os.arch");
    }

    fun getAppDir(): String {
        val fdir = getFilesDir().getAbsolutePath();

        // Extract all resources to the app directory
        val aman = getAssets();
        extractDir(aman, fdir + "/res", "res");
        createIfDoesntExist(fdir + "/modules");

        return fdir;
    }

    fun getBatteryLevel(): String {
        return batteryManager?.getIntProperty(BATTERY_PROPERTY_CAPACITY)?.toString()?:"n/a";
//        return batteryStatusStr;
    }


    fun doPermissionsDialogs() {
        autoPermissionProceed = false;
        proceedWithPermissions();
    }
    fun performHapticFeedback() {
        val vibrator = getSystemService(VIBRATOR_SERVICE) as Vibrator
        if (vibrator != null) {
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
                vibrator.vibrate(
                    VibrationEffect.createOneShot(
                        100,
                        VibrationEffect.DEFAULT_AMPLITUDE
                    )
                )
            } else {
                // Deprecated in API 26
                vibrator.vibrate(100)
            }
        }
    }
}
