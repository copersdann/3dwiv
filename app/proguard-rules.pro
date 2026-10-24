# Правила ProGuard/R8.
# Пока isMinifyEnabled=false. Когда включим минификацию —
# обязательно добавить:
# -keep class com.threewiv.viewer.NativeLib { *; }
# -keepclasseswithmembernames class * { native <methods>; }