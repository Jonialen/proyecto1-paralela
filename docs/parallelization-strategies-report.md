# Comparación de estrategias de paralelización

## Alcance

Se agregaron dos variantes experimentales al renderizador existente sin
modificar la lógica del ejecutable `cubeview` ni la del baseline
`cubeview-seq`:

- `cubeview-task`: estrategia basada en `omp task`.
- `cubeview-nested`: estrategia anidada usada como control negativo.

El archivo `plans/parallelization-strategies.md` indicado para este trabajo no
estaba presente en el repositorio. La implementación sigue las directivas
recibidas en la solicitud y conserva la estrategia plana como comportamiento
predeterminado.

## Estrategia TASK

La geometría crea una tarea por celda de chunk. Después de un `taskwait`, las
celdas se concatenan en orden fijo por vista. El rasterizado crea una tarea por
banda horizontal y el cielo una tarea por segmento de filas.

Las tareas pueden terminar en cualquier orden, pero no comparten buffers de
geometría ni regiones de pantalla. La concatenación ordenada mantiene la
secuencia de triángulos idéntica a la versión plana, por lo que `--dump` sigue
siendo una prueba byte a byte válida.

## Estrategia NESTED

La vista exterior se distribuye con `omp parallel for`. Dentro de cada jugador
se crean equipos OpenMP adicionales para geometría, rasterizado y cielo.
`omp_set_nested(1)` permite que esos equipos internos se formen realmente.

Esta variante es deliberadamente un control negativo: con `N` jugadores cada
vista puede crear un equipo adicional mientras el equipo exterior continúa
activo. El coste esperado es mayor overhead, oversubscription y peor tiempo por
frame frente a la estrategia plana.

Los buffers siguen siendo privados por vista y las bandas de rasterizado son
disjuntas, por lo que la variante no introduce escrituras concurrentes sobre el
mismo píxel.

## Compilación

```sh
make cubeview-task
make cubeview-nested
```

Los objetos de cada variante viven en `build/task/` y `build/nested/`, evitando
mezclar archivos compilados con macros diferentes.

## Comparación recomendada

```sh
./cubeview-seq    -n 4 --view 96 --ssaa 1 --warmup 4 --bench 20
./cubeview        -n 4 --view 96 --ssaa 1 --warmup 4 --bench 20
./cubeview-task   -n 4 --view 96 --ssaa 1 --warmup 4 --bench 20
./cubeview-nested -n 4 --view 96 --ssaa 1 --warmup 4 --bench 20
```

Para comprobar determinismo:

```sh
./cubeview-seq  -n 4 --view 96 --ssaa 2 --dump reference.ppm
./cubeview-task -n 4 --view 96 --ssaa 2 --dump task.ppm
./cubeview-nested -n 4 --view 96 --ssaa 2 --dump nested.ppm
cmp reference.ppm task.ppm
cmp reference.ppm nested.ppm
```

Los resultados numéricos deben generarse en la máquina de medición. El tiempo
de cada etapa se acumula por vista en la variante anidada y por fase en la
variante de tareas; los archivos PPM no incluyen datos variables del reloj.
