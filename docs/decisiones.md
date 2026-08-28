# Registro de decisiones de diseño

Cada entrada explica **qué** se decidió, **por qué**, qué se descartó y qué
consecuencias trajo. La intención es que cualquier integrante del equipo pueda
retomar el proyecto sin tener que reconstruir el razonamiento, y que el informe
pueda citar los motivos en lugar de inventarlos después.

Formato: contexto → decisión → motivo → alternativas descartadas → consecuencias.

---

## D-01. Rasterizador por software en lugar de OpenGL

**Contexto.** El proyecto es un salvapantallas que debe paralelizarse con OpenMP
y demostrar *speedup* y eficiencia.

**Decisión.** Escribir el pipeline gráfico completo en CPU. SDL2 se usa
únicamente para abrir la ventana y volcar el framebuffer terminado.

**Motivo.** Con OpenGL, el trabajo pesado lo hace la GPU. No queda nada que
paralelizar con OpenMP ni nada que medir: el *speedup* mediría la cola de
comandos del driver, no nuestro algoritmo. Un rasterizador propio pone
transformación, recorte, rasterizado, prueba de profundidad y muestreo de
texturas bajo nuestro control.

**Alternativas descartadas.** OpenGL o Vulkan (nada que paralelizar);
*ray tracing* (es otro renderizador entero, riesgo alto cerca de la entrega, y
la rúbrica no lo pide).

**Consecuencias.** Mucho más código propio. A cambio, cada milisegundo del frame
es atribuible a una función nuestra y medible.

---

## D-02. La unidad de paralelismo es la vista, no el triángulo ni el píxel

**Contexto.** Hay que elegir una descomposición y defenderla con el método PCAM.

**Decisión.** `N` exploradores en pantalla dividida. Cada uno es un `ViewTask`:
cámara, viewport, buffer de triángulos propio y puntero al mundo.

**Motivo.** Tres razones que se refuerzan:

1. La rúbrica exige un parámetro `N` que indique *"la cantidad de elementos a
   renderizar"*. `N` exploradores **es** ese parámetro; no hay que inventar uno.
2. Cada viewport es un **rectángulo disjunto** del framebuffer. Dos vistas nunca
   escriben el mismo píxel ni la misma celda de profundidad. La descomposición es
   libre de carreras por construcción: sin cerrojos, sin atómicos, sin reducción.
3. Se explica en cuatro renglones en términos de PCAM.

**Alternativas descartadas.** Paralelizar por triángulo (ver D-07: es una carrera
sobre el z-buffer); un solo explorador con `N` = cantidad de chunks (`N` deja de
ser "elementos a renderizar" y el salvapantallas queda pobre visualmente).

**Consecuencias.** Toda la descomposición vive en un solo bucle de
`render_frame()`. El trabajo geométrico se duplica por vista, pero eso *es* la
carga, y crece de forma predecible con `N`.

---

## D-03. `ViewTask` guarda un puntero al mundo, no un mundo global

**Contexto.** Existe la idea de expandir a `M` submundos independientes, cada uno
con sus propios exploradores, si sobra tiempo.

**Decisión.** El campo es `const World *world`. Hoy todas las vistas apuntan al
mismo mundo.

**Motivo.** Apuntar las vistas a mundos distintos es **el único cambio** que
separa la pantalla dividida de los submundos independientes. El bucle de
renderizado no cambia. Si se hubiera usado un global, esa expansión exigiría
reescribir el pipeline.

**Consecuencias.** Una indirección irrelevante en tiempo de ejecución a cambio de
mantener abierta la extensión.

---

## D-04. Pipeline partido en dos etapas: emisión y rasterizado

**Contexto.** El rasterizador original dibujaba directamente desde la geometría.

**Decisión.** La etapa de geometría (`cube_emit`, `world_emit_view`) **no dibuja
nada**: llena un `TriangleBuffer` con triángulos ya proyectados y acotados. La
etapa de rasterizado (`raster_flush`) consume esa lista.

**Motivo.** Las dos etapas escalan con entradas distintas y se paralelizan de
formas distintas. Sin la separación no se puede ni medirlas por separado ni
aplicarles estrategias diferentes.

**Consecuencias.** Permitió descubrir que **no hay un único cuello de botella**
(ver D-11). También habilita el rasterizado por baldosas: `raster_triangle()`
recibe un rectángulo de recorte y cada `ScreenTriangle` lleva su caja envolvente
ya acotada.

---

## D-05. Mundo con streaming en lugar de grilla fija

**Contexto.** Primero el mundo era una grilla de `size × size` chunks. Se pidió
que el terreno se genere a medida que cada jugador avanza.

**Decisión.** Mundo infinito en X y Z. Los chunks viven en un mapa hash de
direccionamiento abierto indexado por `(cx, cz)`, se generan cuando algún
explorador entra en su radio y se descartan cuando nadie los necesita.

**Motivo.** Con grilla fija los exploradores chocan contra un borde y el mundo
tiene un tamaño máximo. El streaming además crea una fase nueva y medible.

**Alternativas descartadas.** Ventana deslizante centrada (no sirve: los `N`
exploradores están en lugares distintos); lista lineal con búsqueda (O(n) por
consulta, con miles de chunks es inviable).

**Consecuencias.** Coordenadas negativas, y con ellas la necesidad de división
entera hacia abajo (ver `docs/matematica.md` §8.5). También obligó a proteger el
determinismo (D-10).

---

## D-06. Radio de generación = 2.5 × distancia de render

**Decisión.** `CAMERA_STREAM_FACTOR = 2.5f`, por explorador.

**Motivo.** Dos requisitos independientes que el margen satisface a la vez:

1. El descarte de caras en el borde de lo renderizado necesita que el chunk
   vecino **ya exista**; si no, ese borde se dibuja como una pared de caras.
2. Un explorador rápido entraría en chunks aún no generados.

**Consecuencias.** Se generan ~6× más chunks de los que se dibujan. Resultó
barato: el streaming es el 0.2 % del frame en régimen (D-11).

---

## D-07. El desalojo ocurre después de que todos reclaman, con período de gracia

**Decisión.** El orden por frame es `world_begin_frame()` → reclamo de **cada**
explorador → `world_end_frame()` → renderizado.

**Motivo.** Desalojar después de cada explorador tiraría chunks que el siguiente
todavía necesita. El período de gracia (120 frames) evita que un chunk justo en
el borde se genere y se descarte en frames alternos mientras alguien lo roza.

**Detalle.** El techo `--max-chunks` **nunca** desaloja un chunk requerido en el
frame actual. Por eso, con muchos exploradores, el conjunto residente puede
superar el techo de forma legítima. Es un techo blando a propósito: preferimos
gastar memoria antes que abrir un agujero en el terreno.

---

## D-08. Almacenamiento denso de chunks

**Decisión.** `uint8_t blocks[16·32·16]` = 8 KB por chunk, id `0` = aire.

**Motivo.** El enmascarado de caras consulta los seis vecinos de **cada** bloque
sólido. Con almacenamiento denso eso es indexación directa, O(1) y contigua en
memoria. Cualquier representación comprimida (octree, RLE) pagaría esa consulta
muchísimo más caro, y es la consulta más frecuente del programa.

**Consecuencias.** ~50 MB residentes con 16 exploradores. Aceptable.

---

## D-09. Desbalance de carga deliberado

**Decisión.** Cada explorador tiene velocidad propia (`1 + 0.35·i`) y distancia
de render propia. Además parten repartidos en un anillo para que sus caminos no
se superpongan.

**Motivo.** Si todas las vistas costaran lo mismo, `schedule(static)` y
`schedule(dynamic)` darían el mismo resultado y la comparación no diría nada. Con
costos desiguales, la política de planificación de OpenMP se vuelve medible.
El reparto en anillo evita además que varios exploradores compartan chunks, lo
que ocultaría el costo real del streaming para `N` exploradores.

**Evidencia.** En un frame con 4 exploradores: 1600, 3828, 854 y 3525 triángulos.
Diferencia de 4.5× entre la vista más liviana y la más pesada.

---

## D-10. El determinismo es la estrategia de prueba

**Contexto.** No hay *framework* de pruebas y el resultado es una imagen.

**Decisión.** Todo el renderizado es determinista. `--dump` escribe un PPM y dos
corridas con los mismos argumentos producen archivos byte a byte idénticos.

**Motivo.** Da una prueba de corrección objetiva para la versión paralela:

```sh
./cubeview -n 4 --view 96 --ssaa 2 --dump referencia.ppm
cmp referencia.ppm candidato.ppm
```

Si difiere un solo byte, hay una carrera. Sin esto, validar el paralelo sería
mirar la pantalla y opinar.

**Lo que exigió.**

- `world_emit_view()` recorre los chunks con un **barrido ordenado**, no en orden
  del hash. Si no, el orden de los triángulos dependería de cuáles se cargaron
  primero.
- Los volcados sin ventana **no dibujan FPS**. Una lectura de reloj de pared
  difiere en cada corrida.
- Al concatenar buffers por hilo en la versión paralela, **el orden de
  concatenación debe ser fijo** o esta prueba deja de servir.

**Cómo se usó.** Cuando el determinismo se rompió al implementar streaming, se
comparó fila por fila: solo diferían las filas 4 a 13, la línea de FPS del HUD.
El terreno ya era idéntico. Lección: ante un fallo de determinismo, localizar
**qué bytes** difieren antes de auditar el algoritmo.

---

## D-11. Medir por etapa, no estimar

**Decisión.** `--bench` cronometra streaming, geometría y rasterizado por
separado, y corre frames de calentamiento sin cronometrar.

**Motivo.** Un tiempo de frame único esconde dónde está el trabajo. Y los
primeros frames generan miles de chunks de golpe: promediar ese arranque
subestima el rendimiento sostenido.

**Hallazgo.** No hay un único cuello de botella:

| configuración | stream | geometría | raster |
|---|---|---|---|
| `-n 4  --view 96 --ssaa 1` | 0.2 % | **68.1 %** | 30.6 % |
| `-n 4  --view 96 --ssaa 4` | 0.1 % | 19.1 % | **76.8 %** |
| `-n 16 --view 96 --ssaa 1` | 0.2 % | **83.1 %** | 16.3 % |

Con un solo cubo, el rasterizado era el 99.7 % del frame. Con 16 exploradores y
terreno infinito, la geometría es el 83 %. **La misma afirmación, invertida, en
el mismo programa.** Hay que elegir el punto de operación antes de decidir qué
optimizar.

**Nota metodológica.** Un modelo analítico predijo 83 % de geometría con 4
exploradores; la medición dio 68 %. Se mide, no se estima.

---

## D-12. FPS por ventana de tiempo, no por promedio móvil

**Contexto.** El HUD llegó a mostrar 24350 FPS mientras dibujaba 604 000
triángulos.

**Decisión.** Contar frames sobre una ventana fija de 0.35 s.

**Motivo.** Dos errores se potenciaban. La primera iteración medía solo el sondeo
de eventos, así que `1/Δt` daba un número enorme que **sembraba** el promedio. Y
el peso `0.9` era **por frame**, o sea dependiente del frame rate: a 5 FPS
recuerda varios segundos, y la muestra corrupta sobrevivía.

**Lo importante.** Un suavizado por frame **no es comparable entre
configuraciones**. Habríamos comparado secuencial contra paralelo con una métrica
que se comporta distinto según la velocidad. Una ventana de tiempo se comporta
igual a 200 FPS que a 2, y es además la misma fórmula que usa `--bench`.

---

## D-13. HUD sobre la imagen resuelta, con fuente embebida

**Decisión.** Fuente de mapa de bits 5×7 dentro del código fuente, dibujada sobre
el buffer ya resuelto, no sobre el framebuffer supermuestreado.

**Motivo.** La rúbrica exige los FPS **en pantalla**, no en el título de la
ventana. Dibujar el texto antes del filtro de caja lo deja borroso; dibujarlo
después cuesta lo mismo con cualquier `--ssaa`. Embeberla evita distribuir un
archivo de fuente.

---

## D-14. Programación defensiva en la línea de comandos

**Decisión.** Todo argumento se valida con `strtol`, verificando que no queden
caracteres sobrantes y que el valor esté en rango. Se rechaza con mensaje
explícito y código de salida distinto de cero.

**Motivo.** Es criterio explícito de la rúbrica, y recortar valores en silencio
esconde errores: un `--chunks 100` truncado a 8 hizo perder tiempo de
diagnóstico.

---

## D-15. El ciclo día/noche gobierna la luz, no solo el fondo

**Decisión.** Un único valor de fase produce a la vez el degradado del cielo y la
`Light` con que se sombrea el terreno.

**Motivo.** Si el ciclo solo pintara el fondo, de noche quedaría un cielo negro
sobre un terreno iluminado a pleno sol. Por eso la luz pasó de ser un `Vec3` a
una estructura con dirección, ambiente e intensidad: de noche el sol se
reemplaza por una luna tenue en dirección opuesta **y** baja el piso ambiental,
algo que un vector de dirección solo no puede expresar.

**Detalle.** El crepúsculo no es una interpolación entre día y noche: tiene su
propia paleta con horizonte cálido. Interpolando linealmente entre azul de día y
azul de noche nunca aparece un amanecer.

---

## D-16. El cielo se dibuja después del terreno

**Decisión.** `sky_render()` corre al final de `render_view()` y escribe solo
donde el buffer de profundidad sigue en el plano lejano.

**Motivo.** Dibujarlo primero como fondo significa calcular degradado, nubes,
estrellas y brillo del sol para píxeles que el terreno va a tapar. En un frame
típico el terreno cubre la mayor parte del panel, así que se descarta la mayoría
del trabajo sin perder nada.

**Costo medido.** 10.9 ms de 65.6 (16.6 %) con 4 exploradores a `--ssaa 1`. Las
nubes usan intersección del rayo con un plano horizontal, lo que les da
perspectiva real; las estrellas salen de un hash de la dirección cuantizada y no
ocupan memoria.

---

## D-17. Oclusión ambiental por vértice

**Contexto.** El terreno se veía "de plástico": toda cara con la misma
orientación tenía exactamente el mismo brillo, sin importar qué la rodeara.

**Decisión.** Oclusión ambiental por vértice, calculada en la etapa de
geometría. `cube_emit()` recibe un mapa de 27 bits del vecindario 3x3x3 y
oscurece cada esquina según sus tres vecinos (dos de arista, uno diagonal).

**La regla que importa.** Si las **dos** aristas están ocupadas, la esquina está
sellada y la diagonal ya no puede aclararla. Sin ese caso especial, las esquinas
interiores parpadean entre dos tonos según un bloque que ni siquiera se ve.

**Consecuencias.** La luz pasó de ser por triángulo a por vértice: `ScreenVertex`
lleva `l_w` y el rasterizador la interpola con corrección de perspectiva, igual
que las coordenadas de textura. `ClipVertex` también, para que sobreviva al
recorte.

**Costo medido.** El frame pasó de 135.4 a 162.1 ms (+20 %); la geometría de
87.7 a 112.7 ms (+29 %). Es la mejora visual más grande disponible y por eso se
pagó.

**Optimización.** Construir la máscara con comprobaciones de límites por vecino
duplicaba la etapa de geometría. Hay un camino rápido para bloques cuyo vecindario
cae entero dentro del chunk (la enorme mayoría): indexación directa, sin ramas.
Ahorra 14 % y se verificó **byte a byte** contra el camino genérico.

---

## D-18. El terreno se ajusta con datos, no con opiniones

**Contexto.** "El terreno no me convence" no es accionable.

**Decisión.** `--survey N` muestrea el generador sobre un área de N x N bloques e
imprime un histograma de alturas y un censo de biomas.

**Lo que reveló.** El 78 % del mundo estaba entre altura 8 y 19 -- una franja de
11 bloques -- con un máximo de 41 que aparecía en el 0.1 % de los casos. Las
montañas existían y nunca se veían. El 48 % era agua o playa, y `beach_margin=2`
cubría justo las alturas 13-14, que eran el pico exacto de la distribución.

**Causa.** La contribución montañosa era `mask * ridge * amplitud`: un **producto
de dos campos que rara vez son altos a la vez**, así que casi siempre daba casi
cero. Ahora el ridge tiene piso (`0.35 + 0.65*ridge`): donde la máscara dice
"acá hay montañas", hay elevación real, y el ridge solo decide si es cresta o
ladera.

**Segundo hallazgo.** El bioma montaña se elegía por la máscara sola, lo que
pintaba piedra desnuda a nivel del mar. Ahora exige altura real.

**Tercero.** Los campos de temperatura y humedad eran independientes del relieve,
así que aparecía nieve a nivel del mar pegada a un desierto. Se acopló la
temperatura a la altitud (gradiente térmico), que además de ser físicamente
correcto pone el frío en las alturas y los desiertos en las tierras bajas.

**Resultado.** Océano 29 % -> 17 %, playa 19 % -> 6 %, y bosque y llanura pasaron
a dominar. Rango de alturas útil de 5 a 36 en vez de 8 a 19.

---

## D-19. Dos binarios desde las mismas fuentes

**Decisión.** El `Makefile` produce `cubeview` (con `-fopenmp`) y `cubeview-seq`
(sin), a partir del mismo árbol de fuentes. Todo `pragma omp` y toda llamada
`omp_*` está protegida con `#ifdef _OPENMP`.

**Motivo.** La rúbrica pide versión secuencial **y** paralela. Compilar una sola
y fijar los hilos en 1 no es lo mismo: quedaría la maquinaria de OpenMP presente
en el binario. Con la guarda, el secuencial es un programa monohilo genuino.

**Detalle.** `--threads` y `--schedule` se aceptan y se ignoran en el build
secuencial, así la misma línea de comandos sirve para los dos y comparar es
scriptable.

---

## D-20. Una lista plana de tareas por etapa

**Contexto.** Primero se elegía entre partir por vista o partir dentro de una
vista, según `view_count >= threads`.

**Decisión.** Cada etapa arma **una sola lista plana** de tareas `(vista, ítem)`:
un chunk para geometría, una banda de pantalla para rasterizado, una rebanada de
filas para el cielo.

**Motivo.** Elegir un nivel u otro hacía que el speedup fuera un **accidente de
N**: la eficiencia iba de 34 % a 54 % según cómo cayera la división. Partir solo
por vista da tareas gruesas —con dieciséis vistas y un desbalance de 6× la más
pesada define el frame— y partir dentro de una vista por vez paga la entrada a
una región paralela una vez por vista y por etapa.

**Consecuencia.** Eficiencia dentro de cuatro puntos en todo el rango. Cuán fino
se corta cada vista se deriva de la cantidad de hilos, así el total cae siempre
cerca de cuatro tareas por hilo.

**Error cometido.** La primera versión hacía pasar también al build secuencial
por los buffers por celda y la costura. Eso lo volvió 20 % más lento e **infló
el speedup a 4.47×**. Es el tipo de error que halaga: un número que sube porque
empeoró el denominador. De ahí que las tablas lleven siempre las tres columnas
—secuencial, paralelo y FPS— y no solo el speedup.

---

## D-21. Rasterizado por bandas de pantalla, nunca por triángulo

**Decisión.** El rasterizado paralelo parte la pantalla en bandas horizontales;
cada banda es dueña exclusiva de sus filas.

**Motivo.** Dos triángulos pueden cubrir el mismo píxel, y la prueba de
profundidad es una lectura-modificación-escritura de ese único slot compartido.
Repartir por triángulo es una carrera, y de las peores: la imagen sale *casi*
bien, así que se lee como un artefacto de redondeo y no como un error.

**Lo que lo hizo barato.** `raster_triangle()` recibe un rectángulo de recorte
desde la versión secuencial, agregado precisamente para esto. La división por
bandas fueron unas pocas líneas en vez de una reescritura.

---

## D-22. Tres optimizaciones medidas y descartadas

**Decisión.** Se implementaron, se midieron y se revirtieron. Quedan documentadas
en `AGENTS.md` para que nadie las reintente.

| intento | esperado | medido |
|---|---|---|
| reordenar el corte temprano de `raster_triangle` | saltear el 97 % de las llamadas | neutral: 13.28 vs 13.36 ms |
| *binning* de triángulos por banda | 32× menos escaneo | raster −10 %, **frame peor** |
| SIMD por lanes en el bucle de píxeles | 1.3–2× en rasterizado | **26 % más lento** |

**Por qué fallaron.**

- El **reorden** fue neutral porque leer `min_x` ya trae la línea de caché que
  comparten los vértices: la aritmética que salteaba ya era gratis.
- El **binning** perdió porque el escaneo redundante recorre el arreglo de
  triángulos de forma lineal y queda en caché después de la primera banda,
  mientras que el ordenamiento por conteo hace divisiones enteras y escrituras
  dispersas. Reemplazó trabajo gratis por trabajo real.
- El **SIMD** perdió por divergencia. Las caras de bloque son chicas: un lane
  típico tiene dos o tres píxeles cubiertos de ocho, y los otros pagan igual las
  multiplicaciones baricéntricas. Con las aristas ya incrementales, rechazar un
  píxel cuesta tres sumas — no quedaba nada que ahorrar.

**Nota metodológica.** La conclusión inicial sobre SIMD fue *inválida*: se midió
que `-O3 -march=native` no mejoraba y se dedujo que SIMD no serviría. Al
verificar con `-fopt-info-vec-optimized` resultó que el compilador **nunca
vectorizó el bucle interno** —lo dice él mismo: *unsupported control flow*—, así
que esa medición no decía nada sobre SIMD escrito a mano. La premisa no sostenía
la conclusión.

**El patrón de los seis intentos.** Abaratar el camino común le ganó a todos los
intentos de saltearlo. Y un rechazo que parece caro por conteo de instrucciones
puede ser casi gratis cuando recorre memoria en streaming.

---

## Limitaciones conocidas

Documentadas a propósito; están también en el `README.md`.

- **El piso de 30 FPS se cumple hasta ocho exploradores** en la versión
  paralela; con dieciséis quedan 24.9 FPS. La versión secuencial solo lo cumple
  con un explorador.
- **El cielo sigue siendo la etapa más cara** con un explorador, aun después de
  volver gruesa la consulta de nubes. Cachearla por frame en vez de por píxel la
  cortaría más.
- Los primeros frames generan miles de chunks y producen un tirón visible. Un
  presupuesto de generación por frame lo suavizaría.
- No hay descarte por *frustum* de chunks: se recorre el disco completo alrededor
  de cada cámara, incluso lo que queda detrás.
- No hay niebla por distancia, así que el límite de render es un horizonte duro.
- Los árboles sobre una costura de chunks pierden parte de la copa: costo
  habitual de generar chunks de forma independiente.
- El relieve sigue leyéndose suave desde el aire: los exploradores vuelan lo
  bastante alto como para escorzarlo.
