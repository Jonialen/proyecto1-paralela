# Proyecto #1: Screensaver Paralelo con OpenMP

**Universidad del Valle de Guatemala**  
**Facultad de Ingeniería | Departamento de Ciencias de la Computación**  
**Curso:** Computación Paralela y Distribuida (Sección 20)  
**Catedráticos:** Ing. Sebastian Galindo, Ing. Juan Luis Garcia  
**Semestre:** 2-2024  
**Fecha de Entrega:** Semana del 26 al 30 de agosto de 2024 (Fecha límite de entrega de materiales: **6 de septiembre de 2024**)

---

## 1. Antecedentes

OpenMP es una herramienta que nos facilita la transformación de un programa secuencial en uno paralelo, además de permitir la abstracción y programación paralela a alto nivel. Debido a que está pensado como una solución iterativa, podemos realizar cambios graduales en un programa secuencial para aprovechar múltiples recursos mediante ejecución paralela.

---

## 2. Objetivos y Competencias

- Implementar y diseñar un programa para la paralelización de procesos con memoria compartida usando **OpenMP**.
- Aplicar el **método PCAM** y los conceptos de patrones de descomposición y programación para modificar un programa secuencial y volverlo paralelo.
- Realizar mejoras y modificaciones iterativas al programa para obtener mejores versiones de desempeño.

---

## 3. Descripción del Proyecto

En equipos de **3 integrantes**, se debe diseñar e implementar un algoritmo secuencial que resuelva un problema con potencial de ser paralelizado: la creación de un **screensaver** (salvapantallas) animado que corra de forma paralela.

Posteriormente, cada equipo deberá modificar y mejorar el programa buscando incrementar el desempeño del mismo, basándose en los conceptos de **Speedup** y **Eficiencia**.

---

## 4. Requisitos Generales

1. **Lenguaje:** Código de autoría propia en **C / C++** con comentarios explicativos de métodos, variables, etc.
2. **Control de Versiones (Git):** Historial en repositorio privado iniciando como mínimo 2 semanas antes de la entrega. Debe ponerse público el día de la entrega y reflejar trabajo constante (revisión de commits).
3. **Uso de OpenMP:** Aplicado para la paralelización en memoria compartida.
4. **Versiones:** Contar con versión secuencial y versión(es) paralela(s) del algoritmo.

---

## 5. Reglas de Diseño del Screensaver

Para mantener el proyecto en un alcance manejable, se deben cumplir las siguientes reglas:

* **Parámetros:** Recibir por lo menos un parámetro $N$ desde la línea de comandos, que indica la cantidad de elementos a renderizar (ej. cantidad de círculos).
* **Color:** Desplegar varios colores, idealmente generados de forma pseudoaleatoria.
* **Resolución:** Canvas con un tamaño mínimo de **640x480** ($w 	imes h$).
* **Movimiento:** Poseer movimiento constante y ser estéticamente interesante (ej. objetos moviéndose y rebotando en las paredes).
* **Física / Trigonometría:** Incorporar elementos físicos o trigonométricos en los cálculos (ej. cambio de velocidad al rebotar, colisiones, interacción entre elementos $N$).
* **Rendimiento:** Desplegar en pantalla los **FPS** (*frames per second*) en tiempo real para evaluar la experiencia de usuario y garantizar que no caigan por debajo de **30 FPS**.
* **Librería Gráfica:** A elección del equipo (se sugiere **SDL** o **OpenGL**).

---

## 6. Entregables

1. **Informe del Proyecto:** Documento de investigación en formato PDF siguiendo la guía de normas UVG.
2. **Código Fuente:** Archivos `.c` y/o `.cpp` funcionales (sin archivos ejecutables).
3. **Enlace al Repositorio:** Repositorio privado que contenga el historial de commits y el código final (hacer público al momento de evaluar).

*Todo el material debe estar subido antes del periodo de clase del 6 de septiembre.*

---

## 7. Criterios de Evaluación

### A. Informe de Investigación (25%)

| # | Criterio / Contenido | Valor |
|---|---|:---:|
| 1 | **Formato según guía UVG:** Carátula, índice, introducción, antecedentes, cuerpo, citas textuales/pie de página, conclusiones, recomendaciones, apéndice y al menos 3 citas bibliográficas relevantes y confiables. | **10%** |
| 2 | **Anexo 1 - Diagrama de Flujo:** Detalle de todos los pasos (captura de argumentos, solicitud de datos, programación defensiva, secciones paralelas, mecanismos de sincronía y despliegue de resultados). | **5%** |
| 3 | **Anexo 2 - Catálogo de Funciones:** Definición de entradas, salidas y descripción/propósito de cada función, clase o subrutina. | **5%** |
| 4 | **Anexo 3 - Bitácora de Pruebas:** Mínimo 10 mediciones por prueba, resultados obtenidos (eficiencia) y captura de pantalla de las mediciones. | **5%** |

### B. Programa y Presentación (75%)

| # | Criterio / Contenido | Valor |
|---|---|:---:|
| 1 | **Programa Secuencial:** Funciona correctamente, compila sin errores, incluye programación defensiva, evita variables *hard-coded* (recibe argumentos CLI) e incluye `README.md`. | **25%** |
| 2 | **Programa Paralelo:** Funciona correctamente con OpenMP, compila sin errores, aplica programación defensiva, parametriza variables desde argumentos CLI e incluye `README.md`. | **35%** |
| 3 | **Documentación en Código:** Comentarios explicativos sobre las partes esenciales y algoritmos implementados. | **5%** |
| 4 | **Orden y Legibilidad:** Organización del código, nombrado nemotécnico adecuado de variables y funciones. | **5%** |
| 5 | **Sincronización y Memoria:** Uso correcto de mecanismos de protección de memoria compartida, barreras/sincronía y manejo adecuado de inicialización/destrucción de memoria. | **5%** |
