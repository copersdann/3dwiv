// native_bridge.cpp — JNI-мост между Kotlin (com.threewiv.viewer.NativeLib)
// и нативным рендерером/парсерами.
//
// Модель потоков:
//   * UI-поток: касания, кнопки, вызов nativeLoadModel (парсинг вне GL).
//   * GL-поток (GLSurfaceView): nativeInit/nativeResize/nativeRender.
//   * Защита: мьютекс, короткие критические секции; парсинг вне блокировки.
#include <jni.h>
#include <android/log.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "gl_renderer.h"
#include "mesh.h"
#include "stl_parser.h"
#include "threemf_parser.h"
#include "obj_parser.h"
#include "ply_parser.h"
#include "amf_parser.h"
#include "gltf_parser.h"
#include "step_parser.h"

#define LOG_TAG "3dwiv"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

namespace {

std::mutex g_lock;
GlRenderer* g_gl = nullptr;
MeshData g_current;                 // CPU-копия (перезаливка после потери контекста)
std::atomic<bool> g_dirty{false};

bool readFile(const char* path, std::vector<uint8_t>& data) {
    FILE* f = std::fopen(path, "rb");
    if (!f) return false;
    std::fseek(f, 0, SEEK_END);
    const long sz = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (sz <= 0) { std::fclose(f); return false; }
    data.resize(static_cast<size_t>(sz));
    const size_t rd = std::fread(data.data(), 1, data.size(), f);
    std::fclose(f);
    return rd == data.size();
}

std::string lowerOf(const std::string& s) {
    std::string r = s;
    for (char& c : r) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return r;
}

jstring toJString(JNIEnv* env, const std::string& s) {
    return env->NewStringUTF(s.c_str());
}

} // namespace

extern "C" {

// ---------------------------------------------------------------- GL-жизненный цикл

JNIEXPORT jboolean JNICALL
Java_com_threewiv_viewer_NativeLib_nativeInit(JNIEnv*, jclass) {
    std::lock_guard<std::mutex> lk(g_lock);
    if (g_gl) { g_gl->destroy(); delete g_gl; g_gl = nullptr; }
    g_gl = new GlRenderer();
    if (!g_gl->init()) { delete g_gl; g_gl = nullptr; return JNI_FALSE; }
    if (g_current.triangleCount() > 0) g_gl->setMesh(g_current);
    return JNI_TRUE;
}

JNIEXPORT void JNICALL
Java_com_threewiv_viewer_NativeLib_nativeResize(JNIEnv*, jclass, jint w, jint h) {
    std::lock_guard<std::mutex> lk(g_lock);
    if (g_gl) g_gl->resize(static_cast<int>(w), static_cast<int>(h));
}

JNIEXPORT jboolean JNICALL
Java_com_threewiv_viewer_NativeLib_nativeRender(JNIEnv*, jclass) {
    std::lock_guard<std::mutex> lk(g_lock);
    if (!g_gl) return JNI_FALSE;
    if (g_dirty.load()) {
        if (g_current.triangleCount() > 0) g_gl->setMesh(g_current);
        g_dirty.store(false);
    }
    g_gl->render();
    return JNI_TRUE;
}

// ------------------------------------------------------------------ загрузка модели

JNIEXPORT jstring JNICALL
Java_com_threewiv_viewer_NativeLib_nativeLoadModel(JNIEnv* env, jclass, jstring jpath) {
    const char* cpath = env->GetStringUTFChars(jpath, nullptr);
    const std::string path = cpath ? cpath : "";
    if (cpath) env->ReleaseStringUTFChars(jpath, cpath);

    std::string res;
    try {
        std::vector<uint8_t> bytes;
        if (!readFile(path.c_str(), bytes)) {
            res = "Не удалось открыть файл";
        } else {
            MeshData mesh;
            std::string err;
            bool ok = false;

            // -------------------------------------------------- определение
            const std::string lp = lowerOf(path);
            const size_t n = bytes.size();
            auto hasExt = [&](const char* e) {
                const size_t el = std::strlen(e);
                return lp.size() >= el && lp.compare(lp.size() - el, el, e) == 0;
            };
            auto startsWith = [&](const char* s) {
                const size_t sl = std::strlen(s);
                return n >= sl && std::memcmp(bytes.data(), s, sl) == 0;
            };
            bool amfMarker = false;
            if (n > 0) {
                const size_t scanLen = n < 8192 ? n : 8192;
                const char* txt = reinterpret_cast<const char*>(bytes.data());
                if (std::memchr(txt, '<', scanLen)) {
                    for (size_t i = 0; i + 4 < scanLen; i++)
                        if (txt[i] == '<' && txt[i + 1] == 'a' && txt[i + 2] == 'm' &&
                            txt[i + 3] == 'f') { amfMarker = true; break; }
                }
            }
            const bool glbMagic = n >= 4 && bytes[0] == 'g' && bytes[1] == 'l' &&
                                  bytes[2] == 'T' && bytes[3] == 'F';
            const bool zipMagic = n >= 4 && bytes[0] == 'P' && bytes[1] == 'K';
            const bool stepMagic = startsWith("ISO-10303-21");
            const bool plyMagic = startsWith("ply");
            // JSON (текстовый .gltf или его фрагмент) — пробуем как glTF
            bool jsonMagic = false;
            for (size_t i = 0; i < n && i < 512; i++) {
                const char c = static_cast<char>(bytes[i]);
                if (c == ' ' || c == '\t' || c == '\r' || c == '\n') continue;
                jsonMagic = (c == '{' || c == '[');
                break;
            }

            enum class Fmt { NONE, STL, TMF, OBJ, PLY, AMF, GLTF, STEP };
            Fmt fmt = Fmt::NONE;
            if (zipMagic || hasExt(".3mf")) fmt = Fmt::TMF;
            else if (glbMagic || jsonMagic || hasExt(".glb") || hasExt(".gltf")) fmt = Fmt::GLTF;
            else if (plyMagic || hasExt(".ply")) fmt = Fmt::PLY;
            else if (stepMagic || hasExt(".step") || hasExt(".stp")) fmt = Fmt::STEP;
            else if (amfMarker || hasExt(".amf")) fmt = Fmt::AMF;
            else if (hasExt(".stl") || startsWith("solid")) fmt = Fmt::STL;
            else if (hasExt(".obj") || startsWith("v ")) fmt = Fmt::OBJ;
            else if (hasExt(".stl") == false) {
                // без подсказки — пробуем по очереди (drag&drop, «Открыть с помощью»)
                if ((ok = parseObjFile(bytes.data(), n, mesh, err)) ||
                    (ok = parseStlFile(bytes.data(), n, mesh, err)) ||
                    (ok = parseGltfFile(bytes.data(), n, mesh, err)) ||
                    (ok = parsePlyFile(bytes.data(), n, mesh, err)) ||
                    (ok = parseAmfFile(bytes.data(), n, mesh, err)) ||
                    (ok = parseStepFile(bytes.data(), n, mesh, err))) {
                    /* найден */
                }
                fmt = ok ? Fmt::OBJ : Fmt::NONE;
            }

            if (fmt != Fmt::NONE && !ok) {
                switch (fmt) {
                    case Fmt::STL: ok = parseStlFile(bytes.data(), n, mesh, err); break;
                    case Fmt::TMF: ok = parse3mfFile(bytes.data(), n, mesh, err); break;
                    case Fmt::OBJ: ok = parseObjFile(bytes.data(), n, mesh, err); break;
                    case Fmt::PLY: ok = parsePlyFile(bytes.data(), n, mesh, err); break;
                    case Fmt::AMF: ok = parseAmfFile(bytes.data(), n, mesh, err); break;
                    case Fmt::GLTF: ok = parseGltfFile(bytes.data(), n, mesh, err); break;
                    case Fmt::STEP: ok = parseStepFile(bytes.data(), n, mesh, err); break;
                    default: break;
                }
            }

            if (ok && mesh.triangleCount() > 0) {
                std::lock_guard<std::mutex> lk(g_lock);
                g_current = std::move(mesh);
                g_dirty.store(true);
                LOGI("model loaded: %zu tris, %zu verts",
                     g_current.triangleCount(), g_current.vertexCount());
            } else if (!ok) {
                if (err.empty()) {
                    res = "Формат не распознан. Поддерживаются: STL, 3MF, STEP, OBJ, PLY, GLTF/GLB, AMF";
                } else {
                    res = err;
                }
            } else {
                res = "В файле нет треугольной геометрии";
            }
        }
    } catch (const std::exception& e) {
        res = std::string("Ошибка: ") + e.what();
    } catch (...) {
        res = "Неизвестная ошибка";
    }
    return toJString(env, res);
}

JNIEXPORT jstring JNICALL
Java_com_threewiv_viewer_NativeLib_nativeGetStats(JNIEnv* env, jclass) {
    std::lock_guard<std::mutex> lk(g_lock);
    char buf[192];
    if (g_gl && g_gl->hasMesh()) {
        std::snprintf(buf, sizeof buf, "triangles=%u\nvertices=%u\nspan=%.6f\nscale=%.4g",
                      g_gl->triangleCount(), g_gl->vertexCount(),
                      static_cast<double>(g_current.span),
                      static_cast<double>(g_current.scale));
    } else {
        std::snprintf(buf, sizeof buf, "empty");
    }
    return toJString(env, buf);
}

// ------------------------------------------------------------------ управление камерой

JNIEXPORT void JNICALL
Java_com_threewiv_viewer_NativeLib_nativeRotate(JNIEnv*, jclass, jfloat dx, jfloat dy) {
    std::lock_guard<std::mutex> lk(g_lock);
    if (g_gl) g_gl->camera.rotate(static_cast<float>(dx) * 0.2f,
                                  static_cast<float>(dy) * 0.2f);
}

JNIEXPORT void JNICALL
Java_com_threewiv_viewer_NativeLib_nativeZoom(JNIEnv*, jclass, jfloat factor) {
    std::lock_guard<std::mutex> lk(g_lock);
    if (g_gl) g_gl->camera.zoom(static_cast<float>(factor));
}

JNIEXPORT void JNICALL
Java_com_threewiv_viewer_NativeLib_nativePan(JNIEnv*, jclass, jfloat dx, jfloat dy) {
    std::lock_guard<std::mutex> lk(g_lock);
    if (g_gl) g_gl->camera.pan(static_cast<float>(dx), static_cast<float>(dy),
                               static_cast<float>(g_gl->viewH()));
}

JNIEXPORT void JNICALL
Java_com_threewiv_viewer_NativeLib_nativeSetWireframe(JNIEnv*, jclass, jboolean on) {
    std::lock_guard<std::mutex> lk(g_lock);
    if (g_gl) g_gl->wireframe = (on == JNI_TRUE);
}

// ------------------------------------------------------------------ настройки

JNIEXPORT void JNICALL
Java_com_threewiv_viewer_NativeLib_nativeSetAmbient(JNIEnv*, jclass, jfloat v) {
    std::lock_guard<std::mutex> lk(g_lock);
    if (g_gl) g_gl->ambient = v < 0.f ? 0.f : (v > 1.f ? 1.f : v);
}

JNIEXPORT void JNICALL
Java_com_threewiv_viewer_NativeLib_nativeSetFov(JNIEnv*, jclass, jfloat deg) {
    std::lock_guard<std::mutex> lk(g_lock);
    if (g_gl) g_gl->camera.fovDeg = deg < 15.f ? 15.f : (deg > 110.f ? 110.f : deg);
}

JNIEXPORT void JNICALL
Java_com_threewiv_viewer_NativeLib_nativeSetBackground(JNIEnv*, jclass,
                                                       jfloat r, jfloat g, jfloat b) {
    std::lock_guard<std::mutex> lk(g_lock);
    if (g_gl) {
        g_gl->bgR = r < 0.f ? 0.f : (r > 1.f ? 1.f : r);
        g_gl->bgG = g < 0.f ? 0.f : (g > 1.f ? 1.f : g);
        g_gl->bgB = b < 0.f ? 0.f : (b > 1.f ? 1.f : b);
    }
}

JNIEXPORT void JNICALL
Java_com_threewiv_viewer_NativeLib_nativeSetAxes(JNIEnv*, jclass, jboolean on) {
    std::lock_guard<std::mutex> lk(g_lock);
    if (g_gl) g_gl->showAxes = (on == JNI_TRUE);
}

JNIEXPORT void JNICALL
Java_com_threewiv_viewer_NativeLib_nativeResetView(JNIEnv*, jclass) {
    std::lock_guard<std::mutex> lk(g_lock);
    if (g_gl) g_gl->camera.reset();
}

} // extern "C"