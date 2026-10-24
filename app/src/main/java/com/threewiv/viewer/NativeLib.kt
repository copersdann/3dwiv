package com.threewiv.viewer

import kotlin.jvm.JvmStatic

/**
 * JNI-мост к нативной библиотеке "threewiv" (C++/NDK).
 *
 * Все тяжёлые операции — парсинг STL/3MF, расчёт нормалей,
 * загрузка VBO, рендеринг OpenGL ES 3.0 — выполняются в C++.
 * Kotlin здесь только передаёт команды UI.
 *
 * @JvmStatic обязателен: генерирует статические native-методы,
 * чтобы JNI-символы Java_com_threewiv_viewer_NativeLib_* совпадали
 * с экспортами в native_bridge.cpp (сигнатура с jclass).
 */
object NativeLib {

    init {
        System.loadLibrary("threewiv")
    }

    /** Инициализация GL-ресурсов (вызывается из onSurfaceCreated на GL-потоке). */
    @JvmStatic external fun nativeInit(): Boolean

    /** Смена размеров вьюпорта. */
    @JvmStatic external fun nativeResize(w: Int, h: Int)

    /** Отрисовка одного кадра. */
    @JvmStatic external fun nativeRender(): Boolean

    /**
     * Загрузка модели из файла (путь передаёт приложение, скопировав контент
     * из content:// в кэш). Возвращает "" при успехе или текст ошибки.
     * Парсинг выполняется в вызывающем потоке, аплоад в GPU — на GL-потоке.
     */
    @JvmStatic external fun nativeLoadModel(path: String): String

    /** Статистика в виде "triangles=..\nvertices=..". */
    @JvmStatic external fun nativeGetStats(): String

    /** Вращение камеры, dx/dy в пикселях жеста. */
    @JvmStatic external fun nativeRotate(dx: Float, dy: Float)

    /** Зум: factor — отношение предыдущего расстояния к текущему. */
    @JvmStatic external fun nativeZoom(factor: Float)

    /** Панорама цели камеры, dx/dy в пикселях. */
    @JvmStatic external fun nativePan(dx: Float, dy: Float)

    /** Включить/выключить каркасную сетку. */
    @JvmStatic external fun nativeSetWireframe(on: Boolean)

    /** Сброс камеры в исходное положение. */
    @JvmStatic external fun nativeResetView()

    // ------------------------------------------------------------ настройки

    /** Доля рассеянного (ambient) света, 0..1. */
    @JvmStatic external fun nativeSetAmbient(v: Float)

    /** Вертикальный угол обзора камеры, градусы (15..110). */
    @JvmStatic external fun nativeSetFov(deg: Float)

    /** Цвет фона (компоненты 0..1). */
    @JvmStatic external fun nativeSetBackground(r: Float, g: Float, b: Float)

    /** Показывать оси координат XYZ. */
    @JvmStatic external fun nativeSetAxes(on: Boolean)
}