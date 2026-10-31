# Roadmap — SMB1 Classic Multiplayer

## Fase 0 — Base y repositorio

- [x] Crear el repositorio público.
- [x] Partir de SMB Vanilla (GPL-3.0) y conservar créditos.
- [x] Documentar el diseño y las reglas.
- [x] Añadir `.gitignore` para ROMs y builds.
- [x] Compilar el juego base en Windows.
- [x] Probar el modo un jugador con una ROM legal.

## Fase 1 — Base en Windows

- [x] Build reproducible con Meson + SDL3 (o SDL2).
- [x] Configuración de mandito y teclado.
- [x] Pantalla completa con imagen estirada.
- [x] Menú de pausa dentro del juego con opción de volver al título.
- [ ] Primer arranque sin ROM: mensaje claro para seleccionar `smb.nes`.
- [x] Medir rendimiento y estabilidad.

## Fase 2 — Online básico

- [x] Servidor de salas con código de 4 letras.
- [x] Cliente WebSocket en C.
- [x] Conexión de dos clientes y relay de mensajes.
- [x] Estado de conexión en pantalla.
- [ ] Pantalla de lobby: crear/unirse por código (por ahora por línea de comandos).

## Fase 3 — Fantasma remoto

- [ ] Enviar posición/animación del jugador a ~10 Hz.
- [ ] Dibujar el fantasma del otro jugador con interpolación.
- [ ] Indicador cuando el fantasma está fuera de pantalla.
- [ ] Emotes básicos.

## Fase 4 — Poderes compartidos

- [ ] Evento al agarrar un power-up.
- [ ] Aplicar el poder en el mundo del otro jugador.
- [ ] Regla de daño (solo el golpeado pierde el poder).
- [ ] Pruebas con seta, flor y estrella.

## Fase 5 — Bloques eco

- [ ] Marcar bloques eco por nivel y por jugador.
- [ ] Renderizado distintivo.
- [ ] Evento al romperlos.
- [ ] Recompensa o apertura de paso en el otro mundo.

## Fase 6 — Reglas de co-op

- [x] Vidas infinitas.
- [x] Congelar el tiempo y desactivar hurry-up/muerte por tiempo.
- [x] Sin bonus de tiempo al terminar el nivel.
- [ ] Espera en la bandera y avance conjunto.
- [ ] Muerte con respawn al lado del amigo.
- [ ] Si el segundo muere tras terminar el primero, ambos avanzan.
- [ ] Timeout si el que espera se desconecta.

## Fase 7 — Raspberry Pi Zero 2 W

- [ ] Build con SDL2 y render por software (`-Dopengl=false`).
- [ ] Probar 60 fps estables.
- [ ] Mandito por USB/Bluetooth.
- [ ] Lanzador y acceso directo.
- [ ] Empaquetado `.deb` o carpeta portable.

## Fase 8 — Pulido

- [ ] Reconexión.
- [ ] Chat o más emotes.
- [ ] Ajustes de servidor en el menú.
- [ ] Pruebas de latencia y pérdida de paquetes.
- [ ] Documentación de usuario.
