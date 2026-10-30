# Diseño — SMB1 Classic Multiplayer

## 1. Concepto

Dos jugadores online recorren el mismo nivel, pero **cada uno tiene su propia instancia del mundo**. Los enemigos, objetos y bloques pueden estar en estados distintos para cada jugador. El otro jugador se representa como un **fantasma**: se ve su posición y animación, pero no colisiona con nada del mundo local.

La interacción no viene de compartir la física, sino de **eventos**:

- poderes compartidos,
- bloques eco,
- emotes y reanimaciones.

Este modelo evita sincronizar enemigos, física y colisiones, y funciona bien con latencia alta. Está inspirado en el online de fantasmas de *Super Mario Bros. Wonder*.

## 2. Reglas de juego

### Vidas y tiempo

- **Vidas infinitas.** Morir nunca produce Game Over.
- **Sin límite de tiempo.** El contador se congela y se desactivan la música de prisa y la muerte por tiempo.
- Sin bonus de tiempo al terminar el nivel.

### Muerte y reaparición

- Al morir, reapareces **al lado de tu amigo**, en la plataforma segura más cercana a su posición, con 1–2 segundos de invulnerabilidad.
- Si el amigo está en otra sección o nivel, reapareces en tu último checkpoint.
- Si mueren los dos, ambos vuelven al último checkpoint.
- Se lleva un contador de muertes solo como estadística.

### Terminar el nivel

- El primero que llega a la bandera se queda **esperando**.
- Cuando el segundo termina, ambos avanzan.
- Si el segundo muere después de que el primero terminó, **también completa el nivel** y ambos avanzan.
- Si el segundo muere antes de que el primero termine, reaparece y sigue; el primero sigue esperando.
- Si el jugador que espera se desconecta, el otro recibe un timeout para poder terminar solo.

### Cámara

- Cada jugador tiene su propia cámara en su propio dispositivo.
- No hay split-screen.
- El fantasma del otro se ve cuando está dentro de la pantalla; si está lejos, se muestra un indicador en el borde.

## 3. Interacciones online

### Poderes compartidos

- Si un jugador agarra una seta, flor de fuego, estrella u otro poder, se envía un evento.
- El otro jugador recibe el mismo poder en su mundo.
- Si a uno le pegan, **solo ese jugador pierde el poder** (decisión inicial para no frustrar). Pendiente de probar.
- Los poderes no se comparten entre niveles distintos salvo que se decida lo contrario.

### Bloques eco

- Son bloques que existen **solo en el mundo de un jugador**.
- Se distinguen visualmente (por ejemplo, con un brillo o paleta especial).
- Al romperlos, se envía un evento al otro jugador:
  - le aparece una moneda o un power-up,
  - o se le abre un paso en su mundo.
- El bloque roto no desaparece del mundo del otro jugador si allí no existía.
- Los bloques eco se colocan por nivel y por jugador. En el prototipo se marcan posiciones fijas en 1-1; más adelante se puede añadir un editor.

### Emotes y reanimación

- Emotes simples para comunicarse sin chat.
- Si un jugador muere, el otro puede ver su fantasma y, opcionalmente, reanimarlo.
- La reanimación se decide más adelante; la primera versión usa el respawn automático al lado del amigo.

## 4. Red

- **Transporte:** WebSocket contra el servidor de salas/relay.
- **Salas:** código de 4 letras, 2 jugadores.
- **Estado del fantasma:** posición, animación, dirección, personaje y poder, a ~10 Hz.
- **Eventos:** poder compartido, bloque eco, muerte, reanimación, emote, nivel completado.
- **Sin lockstep y sin sincronizar el mundo.** Cada cliente es autoridad de su propia instancia.
- **Interpolación:** el fantasma se suaviza entre paquetes.
- **Reconexión:** fase posterior.

## 5. Plataformas

- **Windows primero.** Es donde se prueba el online y donde es más fácil depurar.
- **Raspberry Pi Zero 2 W después.** Mismo código C/SDL2; render por software y 60 fps objetivo.
- Sin coop local y sin split-screen.

## 6. Legal

- El repositorio **no incluye ROMs ni assets originales de Nintendo**.
- Cada jugador debe aportar su propia ROM obtenida legalmente.
- Proyecto fan, no comercial, sin afiliación con Nintendo.
- La licencia del código base es **GPL-3.0**; las modificaciones de este fork también.

## 7. Decisiones pendientes

- ¿El jugador que espera en la bandera se queda quieto o puede moverse como fantasma?
- ¿El poder compartido se pierde para los dos cuando a uno le pegan, o solo para el golpeado?
- ¿Qué recompensa exacta dan los bloques eco (moneda, power-up, abrir paso)?
- ¿Se permite cambiar de personaje entre niveles?
