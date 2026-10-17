# TODO - Mejoras Pendientes

## Sistema de Notificaciones entre Tareas

### Problema Actual
- Las tareas de control (MotorTask, SteerTask, LightsTask) hacen **polling** del mailbox cada 10ms
- Cuando se escribe un comando en el mailbox, la tarea puede tardar hasta 10ms en enterarse
- Solo los comandos de emergencia usan notificaciones directas (`xTaskNotify`). Ojo: hoy la respuesta **no** es <1 ms sino de hasta ~50 ms, porque `MotorTask` lee la notificación con timeout 0 y sigue bloqueando en `vTaskDelay()` (ver A-1 en `docs/PLAN_MEJORAS.md`)
- Los comandos normales (SET_SPEED, SET_STEER, etc.) tienen delay innecesario

### Solución Propuesta
Implementar notificaciones de FreeRTOS para comandos normales:
- **Opción recomendada**: El productor (quien escribe en el mailbox) debe notificar a la tarea correspondiente
  - Semánticamente correcto: quien genera el evento lo comunica
  - Requiere exponer los handles de las tareas (similar a `motor_task_trigger_emergency()`)
  - Requiere pasar los handles a los productores (link_rx_task, web_task, supervisor_task)
  - Después de cada `mailbox_write()`, llamar `xTaskNotify()` correspondiente

- Las tareas deben cambiar de polling a espera de notificaciones:
  - Cambiar `vTaskDelay()` por `xTaskNotifyWait()` con timeout
  - Mantener timeout como fallback para evitar bloqueo indefinido

### Beneficios
- Reducir latencia de comandos normales (de ~10ms a <1ms)
- Consistencia: todos los comandos usan el mismo mecanismo de notificación
- Mejor uso de CPU: las tareas se bloquean esperando eventos en lugar de hacer polling

### Archivos a Modificar
- `src/motor_task.cpp`, `src/steer_task.cpp`, `src/lights_task.cpp`: Cambiar polling a notificaciones
- `src/motor_task.h`, `src/steer_task.h`, `src/lights_task.h`: Exponer handles de tareas
- `src/link_rx_task.cpp`, `src/web_task.cpp`, `src/supervisor_task.cpp`: Agregar notificaciones después de `mailbox_write()`
- `src/link_rx_task.h`, `src/web_task.h`, `src/supervisor_task.h`: Agregar handles a parámetros
- `src/main.cpp`: Pasar handles a los productores

