# Neon Tune

Plugin de autotune VST3 (FL Studio, Reaper, Ableton, Cubase...). Hecho en C++ con JUCE.

## Como funciona

1. **Deteccion de tono**: YIN sobre el audio diezmado a ~16 kHz, con mediana de 3 estimaciones.
2. **Cuantizacion**: nota mas cercana de la escala elegida (con histeresis para que no salte entre notas).
3. **Cambio de tono**: dos cabezales sobre una linea de retardo con ventana de un numero par de periodos
   (estilo PSOLA), de forma que los saltos de lectura quedan siempre en fase.

## Parametros

| Parametro | Que hace |
|---|---|
| Tonalidad | Tonica de la escala (C..B) |
| Escala | Cromatica, mayor, menor natural/armonica, pentatonicas, blues, dorica |
| Velocidad de correccion | 0 ms = efecto robotico tipo T-Pain. 30-100 ms = natural. 400 ms = muy suave |
| Cantidad | 0-100 %. Cuanto de la correccion se aplica |

## Compilar

### Opcion A: sin instalar nada (GitHub Actions)

1. Sube esta carpeta a un repositorio nuevo de GitHub.
2. Ve a la pestana **Actions**, abre **Compilar VST3** y mira el resultado (o pulsa *Run workflow*).
3. Descarga el artefacto de Windows o macOS.

### Opcion B: en tu ordenador (Windows)

1. Instala Visual Studio 2022 (o Build Tools) con "Desarrollo para el escritorio con C++" y CMake.
2. En esta carpeta:

```
cmake -B build
cmake --build build --config Release --target NeonTune_VST3
```

El plugin queda en `build/NeonTune_artefacts/Release/VST3/Neon Tune.vst3`.

## Instalar en FL Studio (Windows)

1. Copia `Neon Tune.vst3` a `C:\Program Files\Common Files\VST3\`.
2. En FL Studio: *Options > Manage plugins > Find installed plugins*.
3. Ponlo como efecto en una pista de voz.

## Probar el motor sin JUCE

```
g++ -std=c++17 -O2 -o test_core tests/test_core.cpp && ./test_core
```

## Limitaciones conocidas

- Solo voz **monofonica** (una voz a la vez). Acordes o instrumentos polifonicos no funcionan.
- Latencia fija de unos 15 ms (se informa al DAW para que la compense).
- No hay control de formantes: correcciones grandes (mas de 3-4 semitonos) pueden sonar algo "ardilla" o grave.
- La interfaz es la generica de JUCE (sliders simples).
