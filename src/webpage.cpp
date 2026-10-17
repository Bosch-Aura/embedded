const char *webpage = R"html(
<!DOCTYPE html>
<html lang="es">
  <head>
    <title>Control Remoto RC-CAR</title>
    <meta charset="utf-8" />
    <meta name="viewport" content="width=device-width, initial-scale=1.0, maximum-scale=1.0, user-scalable=no" />
    <link href="https://cdn.jsdelivr.net/npm/bootstrap@5.3.0/dist/css/bootstrap.css" rel="stylesheet" />
    <!-- BLOQUEANTE-3: the jQuery CDN <script> that used to live here is gone.
         The ESP32 runs as an isolated AP, so a connected phone has no route to
         the internet: jQuery never loaded, $ was undefined, and EVERY button
         threw - including ARM, DISARM and the emergency brake. All calls now
         use the browser's built-in fetch(), which needs no network beyond the
         car itself. Bootstrap above is cosmetic only; if it fails to load the
         page looks plain but every control still works. -->
    <style>
      #range-slider { width: 90%; height: 20px; }
      #speed-slider-vertical {
        writing-mode: bt-lr;
        -webkit-appearance: slider-vertical;
        width: 60px;
        height: 300px;
        padding: 0 5px;
        margin: 20px 0;
      }
      #speed-slider-vertical::-webkit-slider-thumb {
        -webkit-appearance: slider-thumb-vertical;
        width: 30px;
        height: 30px;
        border-radius: 50%;
        background: #007bff;
        cursor: pointer;
      }
      #speed-slider-vertical::-moz-range-thumb {
        width: 30px;
        height: 30px;
        border-radius: 50%;
        background: #007bff;
        cursor: pointer;
        border: none;
      }
      .speed-slider-container {
        display: flex;
        flex-direction: column;
        align-items: center;
        justify-content: center;
        padding: 20px;
      }
      .speed-label {
        margin-top: 10px;
        font-size: 18px;
        font-weight: bold;
        color: #333;
      }
      .btn-animate { transition: transform 0.2s ease; }
      .btn-animate:active { transform: scale(0.9); }
      * { touch-action: manipulation; }
      #steering-wheel-container {
        display: flex;
        flex-direction: column;
        align-items: center;
        justify-content: center;
        padding: 20px;
      }
      #steering-wheel {
        width: 200px;
        height: 200px;
        border: 4px solid #333;
        border-radius: 50%;
        background: linear-gradient(135deg, #666 0%, #333 100%);
        cursor: pointer;
        touch-action: none;
        position: relative;
        box-shadow: 0 8px 16px rgba(0,0,0,0.3);
      }
      #steering-wheel:active {
        box-shadow: 0 4px 8px rgba(0,0,0,0.3);
      }
      .wheel-spoke {
        position: absolute;
        width: 4px;
        height: 80px;
        background: #fff;
        left: 50%;
        top: 10px;
        transform-origin: 50% 90px;
        transform: translateX(-50%);
      }
      .wheel-center {
        position: absolute;
        width: 40px;
        height: 40px;
        background: #000;
        border-radius: 50%;
        left: 50%;
        top: 50%;
        transform: translate(-50%, -50%);
        border: 3px solid #fff;
      }
      #steering-angle-display {
        margin-top: 10px;
        font-size: 18px;
        font-weight: bold;
        color: #333;
      }
    </style>
  </head>
  <body style="height: 100vh; background-color: white; display: flex; align-items: center; justify-content: center;">
    <div class="cont" style="display: grid; grid-template-columns: repeat(3); grid-template-rows: repeat(3);">
      <!--Arm / Disarm / E-STOP. The system boots DISARMED (C-3): nothing moves until ARM.-->
      <div style="grid-row: 0; grid-column: 1 / span 3; text-align: center; margin-bottom: 10px;">
        <button class="btn btn-success btn-lg" style="font-size: 1.1rem; padding: 10px 18px" onclick='makeAjaxCall("arm")'>ARMAR</button>
        <button class="btn btn-secondary btn-lg" style="font-size: 1.1rem; padding: 10px 18px" onclick='makeAjaxCall("disarm")'>DESARMAR</button>
        <button class="btn btn-danger btn-lg" style="font-size: 1.1rem; padding: 10px 18px" onclick='emergencyBrake()'>E-STOP</button>
        <div id="system-status" style="margin-top: 6px; font-weight: bold;">Estado: --</div>
      </div>
      <!--LightsOff-->
      <div style="grid-row: 1; grid-column: 1; text-align: center; margin-bottom: 10px;">
        <button title="lightOff" class="btn btn-warning btn-lg btn-animate" style="font-size: 1.5rem; padding: 20px 25px" ontouchstart='makeAjaxCall("LightsOff")'>
          <svg xmlns="http://www.w3.org/2000/svg" width="32" height="32" fill="currentColor" viewBox="0 0 16 16">
            <path d="M2 6a6 6 0 1 1 10.174 4.31c-.203.196-.359.4-.453.619l-.762 1.769A.5.5 0 0 1 10.5 13h-5a.5.5 0 0 1-.46-.302l-.761-1.77a1.964 1.964 0 0 0-.453-.618A5.984 5.984 0 0 1 2 6zm3 8.5a.5.5 0 0 1 .5-.5h5a.5.5 0 0 1 0 1l-.224.447a1 1 0 0 1-.894.553H6.618a1 1 0 0 1-.894-.553L5.5 15a.5.5 0 0 1-.5-.5z"/>
          </svg>
        </button>
      </div>
      <!--LightsOn-->
      <div style="grid-row: 1; grid-column: 2; text-align: center">
        <button title="lightOn" class="btn btn-warning btn-lg btn-animate" style="font-size: 1.5rem; padding: 20px 25px" ontouchstart='makeAjaxCall("LightsOn")'>
          <svg xmlns="http://www.w3.org/2000/svg" width="32" height="32" fill="currentColor" viewBox="0 0 16 16">
            <path d="M2 6a6 6 0 1 1 10.174 4.31c-.203.196-.359.4-.453.619l-.762 1.769A.5.5 0 0 1 10.5 13a.5.5 0 0 1 0 1 .5.5 0 0 1 0 1l-.224.447a1 1 0 0 1-.894.553H6.618a1 1 0 0 1-.894-.553L5.5 15a.5.5 0 0 1 0-1 .5.5 0 0 1 0-1 .5.5 0 0 1-.46-.302l-.761-1.77a1.964 1.964 0 0 0-.453-.618A5.984 5.984 0 0 1 2 6zm6-5a5 5 0 0 0-3.479 8.592c.263.254.514.564.676.941L5.83 12h4.342l.632-1.467c.162-.377.413-.687.676-.941A5 5 0 0 0 8 1z"/>
          </svg>
        </button>
      </div>
      <!--LightsAuto-->
      <div style="grid-row: 1; grid-column: 3; text-align: center">
        <button class="btn btn-warning btn-lg btn-animate" style="font-size: 1rem; padding: 26px 20px" ontouchstart='makeAjaxCall("LightsAuto")'>
          <label class="fw-bold">AUTO</label>
        </button>
      </div>

      <!--Speed Slider Vertical-->
      <div class="speed-slider-container" style="grid-row: 2; grid-column: 1;">
        <h4>Velocidad</h4>
        <input type="range" min="-255" max="255" value="0" id="speed-slider-vertical" 
               oninput='updateSpeed(this.value)' />
        <div class="speed-label" id="speed-display">Detenido: 0</div>
      </div>

      <!--Steering Wheel-->
      <div id="steering-wheel-container" style="grid-row: 2; grid-column: 2 / span 2;">
        <div id="steering-wheel">
          <div class="wheel-spoke" style="transform: translateX(-50%) rotate(0deg);"></div>
          <div class="wheel-spoke" style="transform: translateX(-50%) rotate(72deg);"></div>
          <div class="wheel-spoke" style="transform: translateX(-50%) rotate(144deg);"></div>
          <div class="wheel-spoke" style="transform: translateX(-50%) rotate(216deg);"></div>
          <div class="wheel-spoke" style="transform: translateX(-50%) rotate(288deg);"></div>
          <div class="wheel-center"></div>
        </div>
        <div id="steering-angle-display">Center: 105°</div>
      </div>
    </div>

    <script>
      // BLOQUEANTE-3: no jQuery. fetch() is built into every browser that can
      // render this page, so the controls keep working on an offline AP.
      function makeAjaxCall(url) {
        return fetch(url, { cache: 'no-store' }).catch(function () { /* link lost: the car brakes on its own */ });
      }
      
      // Steering wheel control
      const SERVO_CENTER = 105;
      const SERVO_LEFT = 50;
      const SERVO_RIGHT = 160;
      const wheel = document.getElementById('steering-wheel');
      const angleDisplay = document.getElementById('steering-angle-display');
      let isDragging = false;
      let currentAngle = SERVO_CENTER;
      let wheelRotation = 0;
      
      function angleToServo(angleDeg) {
        // Convert wheel rotation (-180 to +180 degrees) to servo angle (50 to 160)
        // Center (0°) = 105, Left (-90°) = 50, Right (+90°) = 160
        const normalized = Math.max(-90, Math.min(90, angleDeg));
        const servoAngle = SERVO_CENTER + Math.round(normalized * (SERVO_RIGHT - SERVO_CENTER) / 90);
        return Math.max(SERVO_LEFT, Math.min(SERVO_RIGHT, servoAngle));
      }
      
      function updateSteering(servoAngle) {
        currentAngle = servoAngle;
        const displayAngle = servoAngle - SERVO_CENTER;
        angleDisplay.textContent = displayAngle === 0 ? 'Center: 105°' : 
          (displayAngle > 0 ? 'Right: ' + servoAngle + '°' : 'Left: ' + servoAngle + '°');
        makeAjaxCall('steer?angle=' + servoAngle);
      }
      
      function getAngleFromEvent(e) {
        const rect = wheel.getBoundingClientRect();
        const centerX = rect.left + rect.width / 2;
        const centerY = rect.top + rect.height / 2;
        const clientX = e.touches ? e.touches[0].clientX : e.clientX;
        const clientY = e.touches ? e.touches[0].clientY : e.clientY;
        const dx = clientX - centerX;
        const dy = clientY - centerY;
        return Math.atan2(dy, dx) * 180 / Math.PI;
      }
      
      wheel.addEventListener('mousedown', function(e) {
        isDragging = true;
        wheelRotation = getAngleFromEvent(e);
      });
      
      wheel.addEventListener('touchstart', function(e) {
        e.preventDefault();
        isDragging = true;
        wheelRotation = getAngleFromEvent(e);
      });
      
      document.addEventListener('mousemove', function(e) {
        if (!isDragging) return;
        const newAngle = getAngleFromEvent(e);
        const delta = newAngle - wheelRotation;
        wheelRotation = newAngle;
        const currentRotation = parseFloat(wheel.style.transform.replace('rotate(', '').replace('deg)', '')) || 0;
        const newRotation = currentRotation + delta;
        wheel.style.transform = 'rotate(' + newRotation + 'deg)';
        updateSteering(angleToServo(newRotation));
      });
      
      document.addEventListener('touchmove', function(e) {
        if (!isDragging) return;
        e.preventDefault();
        const newAngle = getAngleFromEvent(e);
        const delta = newAngle - wheelRotation;
        wheelRotation = newAngle;
        const currentRotation = parseFloat(wheel.style.transform.replace('rotate(', '').replace('deg)', '')) || 0;
        const newRotation = currentRotation + delta;
        wheel.style.transform = 'rotate(' + newRotation + 'deg)';
        updateSteering(angleToServo(newRotation));
      });
      
      document.addEventListener('mouseup', function() {
        if (isDragging) {
          isDragging = false;
          // Return to center
          wheel.style.transition = 'transform 0.3s ease';
          wheel.style.transform = 'rotate(0deg)';
          updateSteering(SERVO_CENTER);
          setTimeout(() => { wheel.style.transition = ''; }, 300);
        }
      });
      
      document.addEventListener('touchend', function() {
        if (isDragging) {
          isDragging = false;
          // Return to center
          wheel.style.transition = 'transform 0.3s ease';
          wheel.style.transform = 'rotate(0deg)';
          updateSteering(SERVO_CENTER);
          setTimeout(() => { wheel.style.transition = ''; }, 300);
        }
      });
      
      // Speed slider control
      // Note: slider vertical with writing-mode bt-lr has min at top, max at bottom
      // So we invert the value: positive slider value = backward, negative = forward
      // We'll invert it so top = forward (positive), bottom = backward (negative)
      let lastSpeedCall = 'changeSpeed?speed=0';

      function updateSpeed(value) {
        const sliderValue = parseInt(value);
        // Invert: slider goes -255 (top) to 255 (bottom), but we want top=forward, bottom=backward
        const speed = -sliderValue; // Invert so top (slider -255) becomes forward (+255)
        const speedDisplay = document.getElementById('speed-display');

        if (speed === 0) {
          speedDisplay.textContent = 'Detenido: 0';
          lastSpeedCall = 'changeSpeed?speed=0';
          throttleActive = false;
        } else if (speed > 0) {
          speedDisplay.textContent = 'Adelante: ' + speed;
          lastSpeedCall = 'changeSpeed?speed=' + speed + '&direction=forward';
          throttleActive = true;
        } else {
          speedDisplay.textContent = 'Atrás: ' + Math.abs(speed);
          lastSpeedCall = 'changeSpeed?speed=' + Math.abs(speed) + '&direction=backward';
          throttleActive = true;
        }
        makeAjaxCall(lastSpeedCall);
      }

      function emergencyBrake() {
        const speedSlider = document.getElementById('speed-slider-vertical');
        if (speedSlider) { speedSlider.value = 0; }
        lastSpeedCall = 'changeSpeed?speed=0';
        throttleActive = false;
        document.getElementById('speed-display').textContent = 'Detenido: 0';
        makeAjaxCall('brake');
      }

      // Keep-alive. Motor commands now expire after CONTROL_CMD_TTL_MS (200 ms) and the
      // vehicle brakes (C-1), and the MANUAL watchdog faults after 1 s without a heartbeat
      // (C-4). Re-sending the current speed at 10 Hz is what keeps web control alive; if
      // the browser is closed or the Wi-Fi drops, the car stops on its own. That is the
      // intended fail-safe, not a bug.
      // BLOQUEANTE-1: when the throttle is at zero we ping /heartbeat instead of
      // re-sending the speed. The old page re-sent changeSpeed?speed=0 at 10 Hz,
      // which the firmware turned into a CMD_STOP - re-arming the 5 s cooldown
      // on every single tick, so the vehicle was locked out while the slider sat
      // at zero and stayed locked 5 s after the operator moved it.
      let throttleActive = false;

      setInterval(function() {
        if (throttleActive) {
          makeAjaxCall(lastSpeedCall);
        } else {
          makeAjaxCall('heartbeat');
        }
      }, 100);

      // Poll the system state so the operator can see whether the car is armed.
      // Note: /status deliberately does NOT feed the watchdog.
      setInterval(function() {
        fetch('status', { cache: 'no-store' })
          .then(function (r) { return r.json(); })
          .then(function (d) {
            document.getElementById('system-status').textContent =
              'Estado: ' + d.state + ' / ' + d.mode;
          })
          .catch(function () {
            document.getElementById('system-status').textContent = 'Estado: SIN CONEXIÓN';
          });
      }, 500);

      // Keyboard controls
      document.addEventListener("keydown", function(event) {
        const speedSlider = document.getElementById('speed-slider-vertical');
        if (!speedSlider) return;
        
        if (event.keyCode == 38) { // Up arrow - forward (decrease slider value, then invert)
          const currentSpeed = parseInt(speedSlider.value);
          if (currentSpeed > -255) {
            speedSlider.value = Math.max(-255, currentSpeed - 10);
            updateSpeed(speedSlider.value);
          }
        } else if (event.keyCode == 40) { // Down arrow - backward (increase slider value, then invert)
          const currentSpeed = parseInt(speedSlider.value);
          if (currentSpeed < 255) {
            speedSlider.value = Math.min(255, currentSpeed + 10);
            updateSpeed(speedSlider.value);
          }
        }
      });
    </script>
  </body>
</html>
)html";



