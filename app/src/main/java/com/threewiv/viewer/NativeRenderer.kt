package com.threewiv.viewer

import android.opengl.GLSurfaceView
import javax.microedition.khronos.egl.EGLConfig
import javax.microedition.khronos.opengles.GL10

/**
 * Рендерер GLSurfaceView: все вызовы делегируются в нативный код.
 * Выполняется на отдельном GL-потоке.
 */
class NativeRenderer : GLSurfaceView.Renderer {

    override fun onSurfaceCreated(gl: GL10?, config: EGLConfig?) {
        NativeLib.nativeInit()
    }

    override fun onSurfaceChanged(gl: GL10?, width: Int, height: Int) {
        NativeLib.nativeResize(width, height)
    }

    override fun onDrawFrame(gl: GL10?) {
        NativeLib.nativeRender()
    }
}