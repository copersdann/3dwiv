package com.threewiv.viewer

import android.app.Activity
import android.content.ContentValues
import android.content.Intent
import android.content.SharedPreferences
import android.graphics.Bitmap
import android.media.MediaScannerConnection
import android.net.Uri
import android.opengl.GLSurfaceView
import android.os.Build
import android.view.PixelCopy
import android.os.Bundle
import android.os.Environment
import android.os.Handler
import android.os.Looper
import android.provider.MediaStore
import android.view.MotionEvent
import android.view.View
import android.view.WindowInsets
import android.widget.Button
import android.widget.ImageButton
import android.widget.SeekBar
import android.widget.Switch
import android.widget.TextView
import android.widget.Toast
import java.io.File
import java.io.FileOutputStream
import java.util.Locale
import javax.microedition.khronos.egl.EGL10
import javax.microedition.khronos.egl.EGLConfig
import javax.microedition.khronos.egl.EGLDisplay
import kotlin.math.abs
import kotlin.math.sqrt

/**
 * Main UI: GLSurfaceView / OpenGL ES 3.0 (C++ ядро) + красивый тёмный
 * оверлей: стат-чип, плавающие кнопки масштаба, нижняя панель действий,
 * bottom-sheet настроек (SharedPreferences — всё сохраняется).
 *
 * Жесты:
 *   1 палец — вращение (чувствительность из настроек)
 *   2 пальца — масштаб + панорама
 *   двойной тап — быстрое приближение
 */
class MainActivity : Activity() {

    private lateinit var glView: GLSurfaceView
    private lateinit var statsText: TextView
    private lateinit var fileText: TextView
    private lateinit var prefs: SharedPreferences

    private lateinit var settingsScrim: View
    private lateinit var settingsPanel: View
    private lateinit var topBar: View
    private lateinit var bottomBar: View
    private lateinit var zoomCluster: View

    // Кнопки настроек (для подсветки активного)
    private lateinit var btnBgDark: Button
    private lateinit var btnBgLight: Button
    private lateinit var btnBgGraphite: Button
    private lateinit var btnMsaaOff: Button
    private lateinit var btnMsaa2: Button
    private lateinit var btnMsaa4: Button

    private var settingsOpen = false
    private var currentFile: String? = null

    // Жесты
    private var gesture = 0
    private var lastX = 0f
    private var lastY = 0f
    private var prevPinch = 0f
    private var midX = 0f
    private var midY = 0f
    private var wireframe = false
    private var lastTapTime = 0L
    private var lastTapX = 0f
    private var lastTapY = 0f

    // Настройки (кэш)
    private var rotSens = 1.0f

    companion object {
        private const val REQ_OPEN = 1001
        private const val KEY_BG = "key_bg"
        private const val KEY_AMBIENT = "key_ambient"
        private const val KEY_FOV = "key_fov"
        private const val KEY_AXES = "key_axes"
        private const val KEY_MSAA = "key_msaa"
        private const val KEY_SENS = "key_sens"
        private const val KEY_AUTOFIT = "key_autofit"

        private fun dp(v: Activity, d: Int): Int =
            (d * v.resources.displayMetrics.density).toInt()

        private fun clamp01(v: Float): Float = when {
            v < 0f -> 0f
            v > 1f -> 1f
            else -> v
        }
    }

    // ------------------------------------------------------------- lifecycle

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_main)

        prefs = getSharedPreferences("3dwiv_settings", MODE_PRIVATE)
        glView = findViewById(R.id.glView)
        statsText = findViewById(R.id.statsText)
        fileText = findViewById(R.id.fileText)
        settingsScrim = findViewById(R.id.settingsScrim)
        settingsPanel = findViewById(R.id.settingsPanel)
        topBar = findViewById(R.id.topBar)
        bottomBar = findViewById(R.id.bottomBar)
        zoomCluster = findViewById(R.id.zoomCluster)

        // ---- GL-поверхность с MSAA из настроек ----
        val msaa = prefs.getInt(KEY_MSAA, 0)
        glView.setEGLContextClientVersion(3)
        glView.setEGLConfigChooser(MsaaChooser(msaa))
        glView.setPreserveEGLContextOnPause(true)
        glView.setRenderer(NativeRenderer())
        glView.renderMode = GLSurfaceView.RENDERMODE_WHEN_DIRTY
        glView.setOnTouchListener { _, ev -> onTouch(ev) }

        // ---- Кнопки главного экрана ----
        findViewById<ImageButton>(R.id.btnOpen).setOnClickListener { pickFile() }
        findViewById<ImageButton>(R.id.btnFit).setOnClickListener {
            NativeLib.nativeResetView()
            glView.requestRender()
        }
        findViewById<ImageButton>(R.id.btnGrid).setOnClickListener {
            wireframe = !wireframe
            NativeLib.nativeSetWireframe(wireframe)
            findViewById<ImageButton>(R.id.btnGrid).alpha = if (wireframe) 1f else 0.55f
            glView.requestRender()
        }
        findViewById<ImageButton>(R.id.btnShot).setOnClickListener { takeScreenshot() }
        findViewById<ImageButton>(R.id.btnSettings).setOnClickListener { setSettings(true) }
        findViewById<ImageButton>(R.id.zoomUp).setOnClickListener {
            NativeLib.nativeZoom(1f / 1.35f)
            glView.requestRender()
        }
        findViewById<ImageButton>(R.id.zoomDown).setOnClickListener {
            NativeLib.nativeZoom(1.35f)
            glView.requestRender()
        }
        settingsScrim.setOnClickListener { setSettings(false) }
        findViewById<Button>(R.id.btnCloseSettings).setOnClickListener { setSettings(false) }

        // ---- Настройки: ссылки на элементы ----
        btnBgDark = findViewById(R.id.btnBgDark)
        btnBgLight = findViewById(R.id.btnBgLight)
        btnBgGraphite = findViewById(R.id.btnBgGraphite)
        btnMsaaOff = findViewById(R.id.btnMsaaOff)
        btnMsaa2 = findViewById(R.id.btnMsaa2)
        btnMsaa4 = findViewById(R.id.btnMsaa4)

        setupSettingsPanel()
        applySavedSettings()

        // ---- Адаптивность: системные барьеры (статус-бар, нав-бар) ----
        if (Build.VERSION.SDK_INT >= 30) {
            window.setDecorFitsSystemWindows(false)
        }
        findViewById<View>(R.id.rootFrame).setOnApplyWindowInsetsListener { v, insets ->
            if (Build.VERSION.SDK_INT >= 30) {
                val sys = insets.getInsets(WindowInsets.Type.systemBars())
                topBar.setPadding(dp(this, 16), dp(this, 10) + sys.top, dp(this, 8), 0)
                bottomBar.setPadding(dp(this, 12), 0, dp(this, 12), dp(this, 10) + sys.bottom)
                zoomCluster.setPadding(0, sys.top, 0, sys.bottom)
            }
            v.onApplyWindowInsets(insets)
        }

        NativeLib.nativeResetView() // триггерит System.loadLibrary
        glView.requestRender()

        handleIntent(intent)
    }

    override fun onNewIntent(intent: Intent) {
        super.onNewIntent(intent)
        setIntent(intent)
        handleIntent(intent)
    }

    private fun handleIntent(intent: Intent?) {
        if (intent?.action == Intent.ACTION_VIEW) {
            val uri = intent.data ?: return
            loadModel(uri, uri.lastPathSegment ?: "model")
        }
    }

    override fun onPause() {
        super.onPause()
        glView.onPause()
    }

    override fun onResume() {
        super.onResume()
        glView.onResume()
    }

    // -------------------------------------------------------------- жесты

    private fun onTouch(ev: MotionEvent): Boolean {
        when (ev.actionMasked) {
            MotionEvent.ACTION_DOWN -> {
                gesture = 1
                lastX = ev.x
                lastY = ev.y
                val now = System.currentTimeMillis()
                if (now - lastTapTime < 300 &&
                    abs(ev.x - lastTapX) < dp(this, 40) &&
                    abs(ev.y - lastTapY) < dp(this, 40)) {
                    // двойной тап — быстрое приближение
                    NativeLib.nativeZoom(0.7f)
                    glView.requestRender()
                    lastTapTime = 0
                } else {
                    lastTapTime = now
                    lastTapX = ev.x
                    lastTapY = ev.y
                }
            }
            MotionEvent.ACTION_POINTER_DOWN -> {
                if (ev.pointerCount == 2) {
                    gesture = 2
                    prevPinch = pinchDist(ev)
                    midX = pinchMidX(ev)
                    midY = pinchMidY(ev)
                }
            }
            MotionEvent.ACTION_MOVE -> when {
                gesture == 1 && ev.pointerCount == 1 -> {
                    NativeLib.nativeRotate((ev.x - lastX) * rotSens, (ev.y - lastY) * rotSens)
                    lastX = ev.x
                    lastY = ev.y
                    glView.requestRender()
                }
                gesture == 2 && ev.pointerCount >= 2 -> {
                    val d = pinchDist(ev)
                    if (prevPinch > 0f) NativeLib.nativeZoom(prevPinch / d)
                    prevPinch = d
                    val x = pinchMidX(ev)
                    val y = pinchMidY(ev)
                    NativeLib.nativePan(x - midX, y - midY)
                    midX = x
                    midY = y
                    glView.requestRender()
                }
            }
            MotionEvent.ACTION_POINTER_UP, MotionEvent.ACTION_UP -> gesture = 0
        }
        return true
    }

    private fun pinchDist(ev: MotionEvent): Float {
        val dx = ev.getX(0) - ev.getX(1)
        val dy = ev.getY(0) - ev.getY(1)
        return sqrt(dx * dx + dy * dy)
    }

    private fun pinchMidX(ev: MotionEvent) = (ev.getX(0) + ev.getX(1)) * 0.5f
    private fun pinchMidY(ev: MotionEvent) = (ev.getY(0) + ev.getY(1)) * 0.5f

    // --------------------------------------------------------------- файлы

    private fun pickFile() {
        try {
            val intent = Intent(Intent.ACTION_OPEN_DOCUMENT).apply {
                addCategory(Intent.CATEGORY_OPENABLE)
                type = "*/*"
            }
            @Suppress("DEPRECATION")
            startActivityForResult(intent, REQ_OPEN)
        } catch (e: Exception) {
            Toast.makeText(this, "Не удалось открыть выбор файла", Toast.LENGTH_LONG).show()
        }
    }

    @Suppress("DEPRECATION")
    override fun onActivityResult(requestCode: Int, resultCode: Int, data: Intent?) {
        super.onActivityResult(requestCode, resultCode, data)
        if (requestCode == REQ_OPEN && resultCode == RESULT_OK) {
            val uri = data?.data ?: return
            loadModel(uri, uri.lastPathSegment ?: "model")
        }
    }

    private fun loadModel(uri: Uri, name: String) {
        currentFile = name
        fileText.text = name
        statsText.text = getString(R.string.loading, name)
        Thread {
            try {
                val file = File(cacheDir, "viewer_model.bin")
                contentResolver.openInputStream(uri)?.use { input ->
                    file.outputStream().use { output -> input.copyTo(output) }
                }
                val err = NativeLib.nativeLoadModel(file.absolutePath)
                runOnUiThread {
                    if (err.isEmpty()) {
                        if (prefs.getBoolean(KEY_AUTOFIT, true)) NativeLib.nativeResetView()
                        statsText.text = formatStats(name)
                        glView.requestRender()
                    } else {
                        statsText.text = "Файл: $name\nОшибка: $err"
                        Toast.makeText(this, err, Toast.LENGTH_LONG).show()
                    }
                }
            } catch (e: Exception) {
                val msg = e.message ?: "Ошибка чтения файла"
                runOnUiThread {
                    statsText.text = "Ошибка: $msg"
                    Toast.makeText(this, msg, Toast.LENGTH_LONG).show()
                }
            }
        }.start()
    }

    private fun formatStats(name: String): String {
        val raw = NativeLib.nativeGetStats()
        var tris = 0L
        var verts = 0L
        var span = 0.0
        for (line in raw.lines()) {
            val kv = line.split("=")
            if (kv.size != 2) continue
            when (kv[0].trim()) {
                "triangles" -> tris = kv[1].trim().toLongOrNull() ?: 0L
                "vertices" -> verts = kv[1].trim().toLongOrNull() ?: 0L
                "span" -> span = kv[1].trim().toDoubleOrNull() ?: 0.0
            }
        }
        val sizeText = if (span > 0.0) {
            String.format(Locale.US, "%.2f %s", span, getString(R.string.unit_u))
        } else "—"
        return "Файл: $name\n" +
            getString(R.string.fmt_tris, tris) + " · " +
            getString(R.string.fmt_verts, verts) + "\n" +
            getString(R.string.fmt_span, sizeText, "")
    }

    // -------------------------------------------------------------- скриншот

    private fun takeScreenshot() {
        if (Build.VERSION.SDK_INT < 26) {
            Toast.makeText(this, R.string.err_screenshot, Toast.LENGTH_LONG).show()
            return
        }
        glView.post {
            val bmp = Bitmap.createBitmap(glView.width, glView.height, Bitmap.Config.ARGB_8888)
            PixelCopy.request(glView, bmp,
                { result ->
                    if (result == PixelCopy.SUCCESS) {
                        saveScreenshot(bmp)
                    } else {
                        Toast.makeText(this, "Не удалось снять кадр (${result})", Toast.LENGTH_LONG).show()
                    }
                },
                Handler(Looper.getMainLooper()))
        }
    }

    private fun saveScreenshot(bmp: Bitmap) {
        val name = "3dwiv_${System.currentTimeMillis()}.png"
        try {
            if (Build.VERSION.SDK_INT >= 29) {
                val values = ContentValues().apply {
                    put(MediaStore.Images.Media.DISPLAY_NAME, name)
                    put(MediaStore.Images.Media.MIME_TYPE, "image/png")
                    put(MediaStore.Images.Media.RELATIVE_PATH, "Pictures/3Dwiv")
                    put(MediaStore.Images.Media.IS_PENDING, 1)
                }
                val uri = contentResolver.insert(MediaStore.Images.Media.EXTERNAL_CONTENT_URI, values)
                if (uri != null) {
                    contentResolver.openOutputStream(uri)?.use { bmp.compress(Bitmap.CompressFormat.PNG, 100, it) }
                    contentResolver.update(uri, ContentValues().apply { put(MediaStore.Images.Media.IS_PENDING, 0) }, null, null)
                    Toast.makeText(this, getString(R.string.shot_saved, "Pictures/3Dwiv/$name"), Toast.LENGTH_LONG).show()
                    return
                }
            }
            // API 24-28: приватная папка приложения (без разрешений)
            val dir = getExternalFilesDir(Environment.DIRECTORY_PICTURES) ?: cacheDir
            val f = File(dir, name)
            FileOutputStream(f).use { bmp.compress(Bitmap.CompressFormat.PNG, 100, it) }
            MediaScannerConnection.scanFile(this, arrayOf(f.absolutePath), null, null)
            Toast.makeText(this, getString(R.string.shot_saved, f.absolutePath), Toast.LENGTH_LONG).show()
        } catch (e: Exception) {
            Toast.makeText(this, "Скриншот: ${e.message}", Toast.LENGTH_LONG).show()
        }
    }

    // -------------------------------------------------------- панель настроек

    private fun setupSettingsPanel() {
        findViewById<SeekBar>(R.id.sbAmbient).setOnSeekBarChangeListener(
            slider { p ->
                NativeLib.nativeSetAmbient(p / 100f)
                findViewById<TextView>(R.id.ambValue).text = "$p%"
                prefs.edit().putInt(KEY_AMBIENT, p).apply()
                glView.requestRender()
            })
        findViewById<SeekBar>(R.id.sbFov).setOnSeekBarChangeListener(
            slider { p ->
                val fov = (15 + p).coerceIn(15, 110)
                NativeLib.nativeSetFov(fov.toFloat())
                findViewById<TextView>(R.id.fovValue).text = "$fov°"
                prefs.edit().putInt(KEY_FOV, fov).apply()
                glView.requestRender()
            })
        findViewById<SeekBar>(R.id.sbSens).setOnSeekBarChangeListener(
            slider { p ->
                rotSens = 0.4f + p * 0.007f
                findViewById<TextView>(R.id.sensValue).text = "$p%"
                prefs.edit().putInt(KEY_SENS, p).apply()
            })
        findViewById<Switch>(R.id.swAxes).setOnCheckedChangeListener { _, on ->
            NativeLib.nativeSetAxes(on)
            prefs.edit().putBoolean(KEY_AXES, on).apply()
            glView.requestRender()
        }
        findViewById<Switch>(R.id.swAutoFit).setOnCheckedChangeListener { _, on ->
            prefs.edit().putBoolean(KEY_AUTOFIT, on).apply()
        }

        btnBgDark.setOnClickListener { setBackgroundPreset(0) }
        btnBgLight.setOnClickListener { setBackgroundPreset(1) }
        btnBgGraphite.setOnClickListener { setBackgroundPreset(2) }

        btnMsaaOff.setOnClickListener { setMsaa(0) }
        btnMsaa2.setOnClickListener { setMsaa(2) }
        btnMsaa4.setOnClickListener { setMsaa(4) }
    }

    private fun slider(onProgress: (Int) -> Unit) = object : SeekBar.OnSeekBarChangeListener {
        override fun onProgressChanged(sb: SeekBar, p: Int, fromUser: Boolean) { onProgress(p) }
        override fun onStartTrackingTouch(sb: SeekBar) {}
        override fun onStopTrackingTouch(sb: SeekBar) {}
    }

    private fun applySavedSettings() {
        // Фон
        val bg = prefs.getInt(KEY_BG, 0)
        setBackgroundPreset(bg, save = false)

        // Свет
        val ambP = prefs.getInt(KEY_AMBIENT, 28)
        findViewById<SeekBar>(R.id.sbAmbient).progress = ambP
        findViewById<TextView>(R.id.ambValue).text = "$ambP%"
        NativeLib.nativeSetAmbient(ambP / 100f)

        // Угол обзора
        val fov = prefs.getInt(KEY_FOV, 45).coerceIn(15, 110)
        findViewById<SeekBar>(R.id.sbFov).progress = ((fov - 15) * 95 / 95).coerceIn(0, 95)
        findViewById<TextView>(R.id.fovValue).text = "$fov°"
        NativeLib.nativeSetFov(fov.toFloat())

        // Оси
        val axes = prefs.getBoolean(KEY_AXES, false)
        findViewById<Switch>(R.id.swAxes).isChecked = axes
        NativeLib.nativeSetAxes(axes)

        // MSAA
        val msaa = prefs.getInt(KEY_MSAA, 0)
        highlightMsaa(msaa)

        // Чувствительность
        val sens = prefs.getInt(KEY_SENS, 100)
        findViewById<SeekBar>(R.id.sbSens).progress = sens
        rotSens = 0.4f + sens * 0.007f
        findViewById<TextView>(R.id.sensValue).text = "$sens%"

        // Автоподгонка
        findViewById<Switch>(R.id.swAutoFit).isChecked = prefs.getBoolean(KEY_AUTOFIT, true)

        glView.requestRender()
    }

    private fun setBackgroundPreset(id: Int, save: Boolean = true) {
        if (save) prefs.edit().putInt(KEY_BG, id).apply()
        val c = when (id) {
            1 -> floatArrayOf(0.93f, 0.94f, 0.96f)
            2 -> floatArrayOf(0.15f, 0.16f, 0.19f)
            else -> floatArrayOf(0.10f, 0.11f, 0.13f)
        }
        NativeLib.nativeSetBackground(c[0], c[1], c[2])
        btnBgDark.alpha = if (id == 0) 1f else 0.5f
        btnBgLight.alpha = if (id == 1) 1f else 0.5f
        btnBgGraphite.alpha = if (id == 2) 1f else 0.5f
        glView.requestRender()
    }

    private fun setMsaa(samples: Int) {
        prefs.edit().putInt(KEY_MSAA, samples).apply()
        highlightMsaa(samples)
        // Пересоздаём EGL-конфигурацию (сброс сохраняет модель через nativeInit)
        glView.setPreserveEGLContextOnPause(false)
        glView.onPause()
        glView.setEGLConfigChooser(MsaaChooser(samples))
        glView.onResume()
        glView.setPreserveEGLContextOnPause(true)
        glView.requestRender()
    }

    private fun highlightMsaa(samples: Int) {
        btnMsaaOff.alpha = if (samples == 0) 1f else 0.5f
        btnMsaa2.alpha = if (samples == 2) 1f else 0.5f
        btnMsaa4.alpha = if (samples == 4) 1f else 0.5f
    }

    // ----------------------------------------------------- bottom-sheet анимация

    private fun setSettings(open: Boolean) {
        if (settingsOpen == open) return
        settingsOpen = open
        settingsScrim.isClickable = open
        if (open) {
            settingsScrim.visibility = View.VISIBLE
            settingsScrim.alpha = 0f
            settingsPanel.visibility = View.VISIBLE
            settingsPanel.translationY = settingsPanel.height.toFloat().coerceAtLeast(1f)
            settingsPanel.alpha = 0f
            settingsScrim.animate().alpha(1f).setDuration(180).start()
            settingsPanel.animate()
                .translationY(0f).alpha(1f).setDuration(220).start()
        } else {
            settingsScrim.animate().alpha(0f).setDuration(180).start()
            settingsPanel.animate()
                .translationY(settingsPanel.height.toFloat()).alpha(0f)
                .setDuration(200)
                .withEndAction {
                    settingsScrim.visibility = View.GONE
                    settingsPanel.visibility = View.GONE
                }
                .start()
        }
    }

    // ------------------------------------------------------- EGL/MSAA chooser

    /** Классический chooser для указанного числа сэмплов MSAA (0 — выкл). */
    private class MsaaChooser(private val samples: Int) : GLSurfaceView.EGLConfigChooser {
        override fun chooseConfig(egl: EGL10, display: EGLDisplay): EGLConfig {
            val attrs = intArrayOf(
                EGL10.EGL_SURFACE_TYPE, EGL10.EGL_WINDOW_BIT,
                EGL10.EGL_RENDERABLE_TYPE, 0x0004, // EGL_OPENGL_ES2_BIT
                EGL10.EGL_RED_SIZE, 8,
                EGL10.EGL_GREEN_SIZE, 8,
                EGL10.EGL_BLUE_SIZE, 8,
                EGL10.EGL_ALPHA_SIZE, 8,
                EGL10.EGL_DEPTH_SIZE, 16,
                EGL10.EGL_SAMPLE_BUFFERS, if (samples > 0) 1 else EGL10.EGL_DONT_CARE,
                EGL10.EGL_SAMPLES, samples,
                EGL10.EGL_NONE
            )
            val configs = arrayOfNulls<EGLConfig>(1)
            val n = IntArray(1)
            if (!egl.eglChooseConfig(display, attrs, configs, 1, n) ||
                n[0] <= 0 || configs[0] == null
            ) throw RuntimeException("Нет EGL-конфигурации (samples=$samples)")
            return configs[0]!!
        }
    }
}