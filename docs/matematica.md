# Fundamentos matemáticos del renderizador

Documento de referencia para el equipo. Explica la matemática que sostiene cada
etapa del pipeline y por qué está implementada de esa forma. Los nombres de
funciones y archivos remiten al código fuente.

Convención de espacios de coordenadas:

```
modelo --[M]--> mundo --[V]--> ojo --[P]--> recorte --÷w--> NDC --[viewport]--> pantalla
```

- **Mundo**: unidades de bloque. Un bloque mide 1x1x1.
- **Ojo**: la cámara está en el origen mirando hacia `-z`.
- **Recorte** (*clip space*): homogéneo, cuatro componentes, antes de dividir.
- **NDC**: cubo `[-1,1]³` después de dividir por `w`.
- **Pantalla**: píxeles, con `y` creciendo hacia abajo.

---

## 1. Matriz de proyección en perspectiva

`mat4_perspective()` en `src/math3d.c`. Con `f = 1 / tan(fovy/2)`:

```
        | f/aspect   0        0                    0                  |
    P = |    0       f        0                    0                  |
        |    0       0   (far+near)/(near-far)  2·far·near/(near-far) |
        |    0       0       -1                    0                  |
```

La última fila `(0,0,-1,0)` es la clave: hace que `w_clip = -z_ojo`. Como la
cámara mira hacia `-z`, todo lo que está delante tiene `z_ojo < 0` y por lo tanto
`w_clip > 0`. Dividir por `w` es entonces dividir por la distancia a la cámara,
que es exactamente el efecto de perspectiva: lo lejano se encoge.

**Verificación de los planos.** Con `near = 0.1` y `far = 100`, un punto en
`z_ojo = -0.1`:

```
z_clip = -1.002·(-0.1) + (-0.2002) = -0.1      w_clip = 0.1
z_ndc  = -0.1 / 0.1 = -1                        (plano cercano)
```

Y en `z_ojo = -100`:

```
z_clip = -1.002·(-100) - 0.2002 = 100.0        w_clip = 100
z_ndc  = 100 / 100 = 1                          (plano lejano)
```

El rango `[-1, 1]` es la convención de OpenGL y es la que asume el resto del
pipeline.

> **Trampa observada en este proyecto.** Con un `far` fijo demasiado corto, la
> geometría lejana produce `z_ndc > 1`. El buffer de profundidad se inicializa en
> `1.0`, así que esos píxeles fallan la prueba `z < depth` y **no se dibujan**.
> Parece un límite de capacidad del renderizador, pero es recorte. Por eso
> `scene_far_plane()` deriva el plano lejano de la distancia de render.

## 2. Matriz de vista

`mat4_look_at()`. Se construye una base ortonormal a partir de la dirección de
mirada:

```
f = normalize(objetivo - ojo)        (adelante)
s = normalize(f × arriba)            (derecha)
u = s × f                            (arriba corregido)
```

La matriz resultante tiene las filas `s`, `u`, `-f` y las traslaciones
`-s·ojo`, `-u·ojo`, `f·ojo`. Es la inversa de la transformación de la cámara:
en vez de mover la cámara, se mueve el mundo entero en sentido contrario.

El producto cruz `f × arriba` falla si la mirada es paralela al vector arriba
(mirar recto hacia arriba o hacia abajo). En este proyecto el *pitch* está
acotado, así que no ocurre.

## 3. Recorte contra el plano cercano

`clip_near_plane()` en `src/render.c`.

**Por qué hay que recortar antes de dividir.** Un vértice detrás de la cámara
tiene `w_clip < 0`. Al dividir, el punto aparece reflejado al otro lado de la
pantalla y el triángulo se dibuja completamente deformado. No es un artefacto
visual menor: es geometría inventada.

En espacio de recorte, el plano cercano es el conjunto donde `z + w = 0`. Se
define la distancia con signo:

```
d(V) = z_clip(V) + w_clip(V)
```

Un vértice está delante del plano si `d > 0`. Se aplica **Sutherland–Hodgman**
sobre esa única condición: se recorre el polígono por aristas, se conserva cada
vértice con `d > 0`, y cuando una arista cruza el plano se inserta el punto de
corte interpolado con

```
t = d(A) / (d(A) - d(B))
```

La interpolación se hace en espacio de recorte, **antes** de dividir, porque ahí
todas las magnitudes siguen siendo lineales. Un cuadrilátero recortado contra un
plano produce como máximo cinco vértices; se triangula en abanico.

## 4. División de perspectiva y mapeo a viewport

`to_screen()`:

```
inv_w = 1 / w_clip
x_pantalla = viewport.x + (x_clip·inv_w · 0.5 + 0.5) · viewport.ancho
y_pantalla = viewport.y + (0.5 - y_clip·inv_w · 0.5) · viewport.alto
```

El `0.5 - ...` en `y` invierte el eje: NDC tiene `y` hacia arriba, la memoria de
video tiene `y` hacia abajo.

Que el mapeo use **el rectángulo del viewport y no el framebuffer completo** es
lo único que convierte el renderizador en pantalla dividida.

## 5. Función de arista, área con signo y descarte de caras traseras

Para dos vértices `a`, `b` y un punto `p`:

```
E(a, b, p) = (b.x - a.x)·(p.y - a.y) - (b.y - a.y)·(p.x - a.x)
```

Es el producto cruz 2D de `(b-a)` y `(p-a)`. Dos propiedades:

- **Su signo** indica de qué lado de la recta `ab` cae `p`.
- **Su magnitud** es el doble del área del triángulo `abp`.

El área con signo del triángulo completo es `E(a, b, c)`.

**Orientación.** Las caras del cubo están definidas en sentido antihorario vistas
desde afuera. Al invertir el eje `y` en el paso anterior, ese sentido se vuelve
horario en pantalla y el área con signo queda **negativa**. Por eso el código
descarta cuando `area >= 0`: eso significa cara trasera o triángulo degenerado.

Ejemplo mínimo: triángulo `(0,0), (1,0), (0,1)` antihorario en `y` hacia arriba.
Tras invertir a un alto `H`: `(0,H), (1,H), (0,H-1)`.

```
E = (1-0)·((H-1)-H) - (0-0)·(0-0) = -1  →  negativo
```

## 6. Coordenadas baricéntricas

Para un píxel `p` dentro del triángulo `abc`:

```
λ0 = E(b, c, p) / E(a, b, c)
λ1 = E(c, a, p) / E(a, b, c)
λ2 = E(a, b, p) / E(a, b, c)
```

Cumplen `λ0 + λ1 + λ2 = 1`. El punto está dentro si las tres tienen el mismo
signo que el área. Como el área es negativa, la prueba de pertenencia es que las
tres funciones de arista sean negativas.

Las funciones de arista son afines en `(x, y)`, así que podrían calcularse de
forma incremental por píxel. La implementación actual las recalcula: es más
legible y no es el cuello de botella.

## 7. Interpolación con corrección de perspectiva

Esta es la parte más delicada del rasterizador.

**El problema.** Las coordenadas de textura `(u, v)` son lineales en el espacio
del objeto, pero **no** en el espacio de pantalla. Interpolarlas directamente con
las baricéntricas produce texturas que se tuercen en diagonal: el error clásico
de la primera PlayStation.

**El resultado.** Bajo proyección en perspectiva, para cualquier atributo `A`
lineal en espacio de ojo, la cantidad que **sí** es lineal en pantalla es `A/w`.
También lo es `1/w`. Por lo tanto:

```
            Σ λᵢ · (Aᵢ / wᵢ)
A(p)  =  ─────────────────────
            Σ λᵢ · (1 / wᵢ)
```

Se interpolan numerador y denominador por separado, y recién al final se divide.
El código guarda `u_w = u/w`, `v_w = v/w`, `l_w = luz/w` e `inv_w = 1/w` en cada
`ScreenVertex`, precisamente para no recalcularlos por píxel. La **luz** entró a
esta lista cuando se agregó oclusión ambiental (§10.2): dejó de ser una constante
por triángulo y pasó a ser un atributo por vértice, sujeto exactamente a la misma
corrección que las coordenadas de textura.

```c
float inv_w = l0*a->inv_w + l1*b->inv_w + l2*c->inv_w;
float w     = 1.0f / inv_w;
float u     = (l0*a->u_w + l1*b->u_w + l2*c->u_w) * w;
```

### 7.1 La profundidad es la excepción

La profundidad **no** necesita corrección, y esto sorprende. La razón:

```
z_ndc = (A·z_ojo + B) / (-z_ojo) = -A - B/z_ojo
```

Es decir, `z_ndc` es una función afín de `1/z_ojo`. Y `1/z_ojo` es lineal en
pantalla. Componer una función afín con algo lineal da algo lineal, así que
`z_ndc` **es lineal en espacio de pantalla** y se interpola directamente:

```c
float z = l0*a->z + l1*b->z + l2*c->z;
```

Esa es exactamente la razón por la que los buffers de profundidad almacenan
`z/w` y no la distancia real. También explica por qué la precisión de
profundidad se concentra cerca de la cámara.

## 8. Ruido de valor y fBm

`src/noise.c`.

### 8.1 Ruido de valor

Se asigna un pseudoaleatorio determinista a cada punto de una retícula entera
(mediante un hash de las coordenadas) y se interpola bilinealmente entre las
cuatro esquinas de la celda.

La interpolación **no** usa la fracción cruda `t`, sino el *smoothstep* de
Hermite:

```
S(t) = 3t² - 2t³ = t²(3 - 2t)
```

**Por qué.** `S'(0) = S'(1) = 0`. Con interpolación lineal la derivada salta al
cruzar cada línea de la retícula, y esa discontinuidad se ve como un enrejado de
rombos sobre el terreno. Con smoothstep la primera derivada es continua entre
celdas y el enrejado desaparece.

### 8.2 fBm (movimiento browniano fraccional)

```
fBm(x, z) = Σ  amplitudᵢ · ruido(x·frecuenciaᵢ, z·frecuenciaᵢ)  /  Σ amplitudᵢ
```

con `frecuenciaᵢ₊₁ = frecuenciaᵢ · lacunarity` y
`amplitudᵢ₊₁ = amplitudᵢ · gain`. En el proyecto: 4 octavas, `lacunarity = 2.0`,
`gain = 0.5`.

La división por la suma de amplitudes normaliza el resultado a `[0, 1]`.

Cada octava usa **una semilla distinta**. Sin eso, todas las capas son el mismo
patrón a distinta escala y aparece una autosemejanza artificial muy visible.

### 8.3 Modelado de la altura

Sobre el fBm se aplica otra vez smoothstep:

```
n' = n²(3 - 2n)
```

Es **simétrico**: aplana los dos extremos y empina el centro, que es justamente
donde se concentra la salida del fBm.

```
S(0.25) = 0.156     (empuja hacia abajo)
S(0.75) = 0.844     (empuja hacia arriba)
```

El efecto es **más contraste** entre valles y crestas, no un sesgo hacia terreno
bajo. (Este documento corrigió un comentario del código que afirmaba lo
contrario.)

La altura final es `base_height + n'·amplitude`, truncada a entero.

### 8.4 Evaluación en coordenadas de mundo

El ruido se evalúa en `chunk_x · 16 + x`, no en la coordenada local del chunk.
Esa decisión es la que permite generar cada chunk de forma independiente, en
cualquier orden, y que aun así encajen sin costura. Es también lo que hace el
streaming posible.

### 8.5 Deformación del dominio (*domain warping*)

El fBm crudo produce manchas redondeadas que se leen como "procedurales". La
deformación del dominio desplaza el **punto de muestreo** usando más ruido antes
de leer el campo de altura:

```
wx = x/escala + (fBm₁(x, z) - 0.5) · fuerza
wz = z/escala + (fBm₂(x, z) - 0.5) · fuerza
h  = fBm(wx, wz)
```

Los dos campos de deformación usan semillas distintas, si no el desplazamiento
sería diagonal y uniforme.

El efecto es que las curvas de nivel se **doblan**: aparecen crestas, ensenadas y
valles que parecen erosionados. Cuesta dos evaluaciones extra de ruido y cambia
por completo el carácter del terreno. Es la técnica más barata que existe para
que el ruido deje de parecer ruido.

### 8.6 Ruido con crestas (*ridged*)

```
r(x, z) = (1 - |2·fBm(x, z) - 1|)²
```

El valor absoluto **pliega** el fBm alrededor de su punto medio: los valores
cercanos a 0.5 se vuelven altos y los extremos se vuelven bajos. Donde el fBm
tenía una transición suave por el medio, ahora hay un pico agudo. Elevar al
cuadrado afila más esas crestas y aplana los valles.

Es la diferencia entre montañas con filo y domos redondeados.

**Combinación con la máscara.** La altura final es:

```
h = base + fBm·amplitud + máscara · (0.35 + 0.65·r) · amplitud_montaña
```

El piso `0.35` bajo el ridge no es cosmético. La versión anterior era
`máscara · r · amplitud_montaña`, un **producto de dos campos que rara vez son
altos a la vez**: el resultado era casi siempre casi cero y las montañas existían
en la fórmula pero no en la pantalla. Con piso, donde la máscara dice "acá hay
montañas" hay elevación real y el ridge solo decide si es cresta o ladera.

### 8.7 Gradiente térmico

Los campos de temperatura y humedad son ruido 2D independiente del relieve. Eso
produce nieve a nivel del mar pegada a un desierto. La corrección es acoplarlos:

```
temperatura_efectiva = temperatura - (altura - nivel_del_mar) · 0.008
```

Es el gradiente térmico atmosférico. Además de ser físicamente correcto, resuelve
las dos cosas a la vez: el frío se va a las alturas y los desiertos a las tierras
bajas.

### 8.8 División entera hacia abajo

Para pasar de coordenada de mundo a coordenada de chunk hace falta división
**hacia abajo**, no la división entera de C, que trunca hacia cero:

```
-1 / 16  =  0    en C          (incorrecto)
floor(-1 / 16) = -1            (correcto)
```

Sin esto, los bloques con coordenada negativa entre `-15` y `-1` se asignan al
chunk `0` y el terreno se raja a lo largo de ambos ejes negativos.
`floor_div()` y `mod_floor()` en `src/world.c`.

## 9. Trayectoria de los exploradores

`src/camera.c`. Curva de Lissajous cerrada, con oscilación vertical independiente:

```
x(t) = cx + Rx · sin(ωx·t·v + φx)
z(t) = cz + Rz · sin(ωz·t·v + φz)
y(t) = h  + Ab · sin(ωb·t·v + φy)
```

donde `v` es la velocidad propia del explorador.

**Cuándo cierra la curva.** Solo cuando ambos senos completan ciclos enteros a la
vez. Con `ωx = 0.085` y `ωz = 0.052`, la razón `85/52` ya está en términos
mínimos, de modo que el período común es

```
T = 2π · 1000 ≈ 6283 s ≈ 1 h 45 min   (a velocidad 1)
```

Suficientemente largo como para que el recorrido nunca se repita de forma
visible. Como `ωx ≠ ωz`, la curva es un ocho y no un círculo, y por eso barre
área en lugar de orbitar un anillo.

**Rumbo.** Se toma de la dirección de avance **horizontal** únicamente:

```
d = (x(t+Δ) - x(t),  z(t+Δ) - z(t))     normalizado
```

Usar la derivada completa de la trayectoria es un error: cerca de los extremos
del seno vertical la velocidad del vaivén domina a la horizontal y la cámara
termina mirando al cielo. El *pitch* es su propia oscilación, sesgada hacia
abajo para mantener el terreno en cuadro.

## 10. Iluminación

### 10.1 Término difuso

Difusa lambertiana, una por cara:

```
I_cara = ambiente + intensidad · max(0, n̂ · l̂)
```

La normal se transforma por la matriz de modelo. Como los bloques solo se
trasladan y nunca rotan, la normal transformada coincide con la normal de la
cara: seis valores posibles.

`ambiente` e `intensidad` **no son constantes**: los fija el ciclo día/noche
(§14). De noche el sol se reemplaza por una luna tenue en dirección opuesta y
baja el piso ambiental. Un vector de dirección solo no puede expresar eso, por
eso la luz es una estructura.

### 10.2 Oclusión ambiental

Con solo el término difuso, **toda cara con la misma orientación tiene
exactamente el mismo brillo**, sin importar qué la rodee. El terreno de voxels se
ve de plástico. La oclusión ambiental es la corrección.

Para cada una de las cuatro esquinas de una cara se miran tres vecinos: los dos
que comparten arista con esa esquina (`lado₁`, `lado₂`) y el que está en
diagonal (`esquina`). Con `1` = sólido:

```
nivel = (lado₁ ∧ lado₂) ? 0 : 3 - (lado₁ + lado₂ + esquina)
sombra = {0.46, 0.65, 0.83, 1.0}[nivel]
```

**El caso especial es lo que importa.** Si los dos vecinos de arista están
ocupados, la esquina está *sellada*: el diagonal ya no puede aclararla porque no
hay camino por donde llegue luz. Sin esa regla, las esquinas interiores parpadean
entre dos tonos según un bloque que el observador ni siquiera puede ver.

**Cómo se obtienen los vecinos.** La cara tiene normal `n` sobre un eje. Los otros
dos ejes son los que la cara *abarca*, y el signo de cada uno lo da la posición
del vértice sobre ese eje. Con `t` y `u` esos dos vectores unitarios con signo:

```
lado₁  = n + t
lado₂  = n + u
esquina = n + t + u
```

El vecindario 3×3×3 se empaqueta en 27 bits con índice
`(dx+1)·9 + (dy+1)·3 + (dz+1)`, así el generador de geometría lo calcula una vez
por bloque visible y el emisor de caras solo consulta bits.

**Consecuencia sobre el rasterizador.** La luz dejó de ser constante por
triángulo: ahora varía entre los vértices, así que se interpola por píxel con la
misma corrección de perspectiva que las coordenadas de textura (§7).

## 11. Supermuestreo

Se renderiza a `ancho·s × alto·s` y se promedia cada bloque de `s×s` muestras:

```
color_final = (1/s²) · Σ muestras
```

Es un filtro de caja. No es el mejor filtro de reconstrucción —su respuesta en
frecuencia deja pasar *aliasing*— pero es el más barato y suficiente aquí. El
costo por píxel crece con `s²`, lo que convierte a `--ssaa` en la perilla de
carga del rasterizador.

## 12. Descarte de caras por vecindad

Una cara entre dos bloques sólidos nunca se ve. Para cada bloque sólido se
consultan sus seis vecinos y se emite solo la cara cuyo vecino es aire.

Si `S` es la cantidad de bloques sólidos, sin descarte se emitirían `6S` caras
(`12S` triángulos). En terreno típico sobrevive un orden de magnitud menos: es la
diferencia entre viable e inviable.

Detalle de implementación: los cuatro chunks vecinos horizontales se resuelven
**una vez por chunk**, no una vez por bloque y por lado. Un vecino ausente se
cuenta como **sólido**, no como aire, para no dibujar una pared de caras en el
borde de la región cargada.

## 13. Radio de generación

El terreno se genera hasta `2.5 ×` la distancia de render de cada explorador. El
factor no es arbitrario:

1. El descarte por vecindad en el borde de lo renderizado necesita que el chunk
   vecino **ya exista**. Si no, ese borde se dibuja como una pared.
2. Un explorador rápido entraría en chunks todavía inexistentes.

Además, la carga se mide sobre un **disco** y no sobre un cuadrado: las esquinas
del cuadrado están hasta un 41 % más lejos (`√2`) y costarían un 27 % más de
chunks (`4/π`) de terreno que nadie alcanza a ver.

---

## 14. Cielo y ciclo día/noche

### 14.1 Reconstrucción del rayo de vista

El cielo necesita saber **hacia dónde mira** cada píxel. Se reconstruye a partir
de la base ortonormal de la cámara, que es la misma que usa la proyección:

```
sx = ndc_x · tan(fov/2) · aspecto
sy = ndc_y · tan(fov/2)
d̂  = normalizar(adelante + derecha·sx + arriba·sy)
```

Es la operación inversa de la proyección de §4: en vez de llevar un punto del
mundo a la pantalla, lleva un píxel de la pantalla a una dirección del mundo.

### 14.2 Posición del sol

```
ángulo = 2π·(fase - 0.25)
sol = normalizar( (cos·0.62,  sin,  cos·0.79) )
```

con `fase` = 0 medianoche, 0.25 amanecer, 0.5 mediodía, 0.75 atardecer. Las
componentes horizontales desiguales inclinan el arco para que el sol no pase
exactamente por el cenit.

La elevación `sin(ángulo)` gobierna todo lo demás: la paleta del cielo, la
visibilidad de las estrellas y los parámetros de `Light` de §10.1.

**El crepúsculo tiene paleta propia**, no es una interpolación entre día y noche.
Interpolando linealmente entre azul de día y azul de noche el naranja no aparece
nunca, porque no está en ninguno de los dos extremos.

### 14.3 Nubes: intersección rayo–plano

Las nubes viven en un plano horizontal a altura `H` sobre la cámara. Para un rayo
con dirección `d̂` desde el ojo `e`, el parámetro de intersección es:

```
s = H / d̂_y            (solo si d̂_y > 0)
p = (e_x + d̂_x·s,  e_z + d̂_z·s)
```

y se muestrea el ruido en `p`. Eso es lo que les da **perspectiva**: como `s`
crece sin límite cuando `d̂_y → 0`, las nubes se comprimen hacia el horizonte y
se abren sobre la cabeza, igual que las de verdad. Pegar una textura plana sobre
el cielo no produce ese efecto.

Cerca del horizonte la intersección se estira tanto que el ruido se convertiría
en bandas, así que se desvanece con `d̂_y`.

### 14.4 Estrellas sin memoria

Se cuantiza la dirección de vista sobre una retícula y se hashea:

```
estrella = hash(⌊d̂ₓ·k⌋, ⌊d̂_y·k⌋, ⌊d̂_z·k⌋) > umbral
```

No hay lista de estrellas ni textura: la posición **es** la consulta. Como
depende solo de la dirección y no de la posición de la cámara, las estrellas
quedan fijas en el cielo cuando el explorador se mueve, que es el comportamiento
correcto para objetos en el infinito.

### 14.5 Por qué el cielo se dibuja al final

`sky_render()` corre **después** del terreno y escribe solo donde el buffer de
profundidad sigue en el plano lejano. Los píxeles que el terreno tapa no se
calculan nunca. En un frame típico el terreno cubre la mayor parte del panel, así
que es la mayoría del trabajo descartado.

Aun así el cielo es caro: a `--ssaa 4` cuesta más que el rasterizador, porque la
consulta de nubes son tres octavas de ruido **por píxel de cielo** y el
supermuestreo las multiplica por 16.

---

## Referencias

- Pineda, J. (1988). *A Parallel Algorithm for Polygon Rasterization*.
  SIGGRAPH '88. — origen del rasterizado por funciones de arista.
- Blinn, J. (1992). *Hyperbolic Interpolation*. IEEE CG&A. — corrección de
  perspectiva.
- Sutherland, I. y Hodgman, G. (1974). *Reentrant Polygon Clipping*. CACM.
- Perlin, K. (1985). *An Image Synthesizer*. SIGGRAPH '85. — ruido y la función
  de interpolación suave.
- Quilez, I. *Domain Warping*. — deformación del dominio aplicada a fBm.
- Musgrave, F. K., Kolb, C. y Mace, R. (1989). *The Synthesis and Rendering of
  Eroded Fractal Terrains*. SIGGRAPH '89. — ruido con crestas y terreno
  fractal.
