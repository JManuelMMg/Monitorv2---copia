# Guía de calibración de precisión — MQ-4, MH-Z19Z, DHT11, JSN-SR04T

Qué cambió en el firmware y cómo usarlo para que las cuatro lecturas se acerquen
lo más posible al valor real, no solo al valor "razonable" del datasheet.

## 1. MQ-4 (CH4) — el cambio de fondo

### El problema que tenía la versión anterior

`getMQ4PPM()` calculaba PPM con `A * ratio^B`, con A=1012.7 y B=-2.786 tomados
directo del datasheet de Winsen/Hanwei. Esa curva la midieron ellos en *su*
banco de pruebas, con *su* muestra de sensor, en aire seco a 20°C. Tu unidad
individual tiene su propia dispersión de fabricación (Rs/R0 varía sensor a
sensor incluso dentro del mismo lote), y encima el biodigestor no es aire
limpio: hay CO2 en concentración alta, humedad cercana a saturación y trazas
de H2S — todo eso desplaza la curva real de tu sensor respecto a la del
datasheet. Calibrar solo R0 en aire limpio corrige el punto de partida (offset
de la curva) pero no corrige la pendiente (B), así que el error crece cuanto
más lejos estés de la concentración de aire limpio.

### Qué se agregó

Un sistema de calibración multipunto: expones el sensor a 2 o más
concentraciones de CH4 *conocidas* y el firmware ajusta A y B por regresión
lineal en escala log-log (`log(ppm) = log(A) + B·log(ratio)`), reemplazando la
curva genérica por una propia de tu sensor + tu instalación. Guarda el
resultado en `Preferences` (memoria no volátil), igual que R0.

### Procedimiento paso a paso

1. **Primero calibra R0** con el sensor en aire limpio real. Para el primer
   uso, deja el sensor encendido y estabilizado durante 2-24 horas, según la
   nota del procedimiento de calibración; el `mq_warmup_ms` de 5 minutos solo
   evita medir inmediatamente tras cada reinicio y no sustituye ese burn-in.
   Desde la web o enviando `{"cmd":"calibrate"}` por serial se inicia la
   calibración. En el Monitor Serial también puedes enviar `c` y Enter. Toma
   100 muestras durante unos 50 segundos, usa `Rs/R0 = 4.85` como factor de
   aire limpio empírico y guarda R0 en memoria no volátil. Ese factor se eligió
   para este montaje; el valor genérico del datasheet es 4.4 y no es universal.
   Se rechazan lecturas inestables (`cv_pct > 25%`) o fuera del rango esperado.
   Si falla, revisa que el sensor esté realmente en aire limpio y repite sin
   corrientes de aire ni movimiento cerca del sensor.

   Para borrar R0 y forzar una calibración nueva, envía `b` y Enter por serial
   o `{"cmd":"reset_mq4_calibration"}`. Esto no borra la curva multipunto.

2. **Consigue puntos de referencia.** Necesitas exponer el sensor a
   concentraciones de CH4 que conozcas con certeza. En orden de preferencia:
   - Bolsa Tedlar o cilindro de gas de calibración de CH4 a concentración
     certificada (la opción más confiable, se consigue en proveedores de
     gases de calibración).
   - Comparación simultánea con un analizador de biogás portátil ya
     calibrado (los que se usan para medir % CH4/CO2 en biodigestores):
     colocás ambos sensores midiendo el mismo gas al mismo tiempo y usás la
     lectura del analizador como referencia.
   - Si no tenés ninguno de los dos, como mínimo usa el biogás real del
     reactor en 2-3 momentos de producción claramente distintos (por ejemplo,
     recién cargado vs. en pico de producción) y estimá la concentración por
     algún método indirecto disponible (cromatografía de un laboratorio
     cercano, si es una sola vez, ya sirve como ancla).

3. **Para cada punto de referencia:**
   - Expón el sensor de forma estable a esa concentración.
   - Envía `{"cmd":"add_calibration_point","ppm":1500}` (con el PPM real,
     no el que muestra el sensor) — o usa el campo "PPM CH4 de referencia" y
     el botón "Agregar Punto" en el dashboard.
   - El firmware toma 20 muestras a lo largo de ~10 segundos y las promedia.
     Esperá a que confirme `{"event":"mq4_curve_point","status":"captured"...}`.
   - Repetí con al menos **2 concentraciones distintas**, idealmente 3, y que
     estén razonablemente separadas entre sí (por ejemplo 200, 1500 y 5000
     PPM, no 1000 y 1100 — puntos muy cercanos hacen la regresión inestable).

4. **Ajusta la curva:** `{"cmd":"fit_calibration_curve"}` o botón "Ajustar
   Curva". Vas a recibir algo como:
   ```json
   {"event":"mq4_curve_fit","status":"completed","puntos":3,"a":842.15,"b":-2.41,"r2":0.97}
   ```
   `r2` es el indicador de qué tan bien ajustan tus puntos a una curva de
   potencia — por encima de 0.90 es buena señal; si sale más bajo, revisá que
   los PPM de referencia sean correctos y repetí con puntos más separados.

5. Si algo salió mal, `{"cmd":"reset_calibration_points"}` borra los puntos
   sin tocar la curva activa, y `{"cmd":"reset_curve"}` vuelve a la curva del
   datasheet como rollback de seguridad.

La curva ajustada queda guardada en memoria no volátil y sobrevive reinicios;
`sendTelemetry()` ahora reporta `mq4_curve_a`, `mq4_curve_b`,
`mq4_curve_custom` y `mq4_curve_points` en cada trama, así que el dashboard
siempre muestra si estás corriendo la curva genérica o la propia.

### Sobre la compensación de temperatura/humedad

`getMQ4CompensationFactor()` sigue siendo un modelo lineal simplificado
(no hay curva de corrección oficial de Winsen para el MQ-4 tan detallada como
la de otros gases). Si notás que el error cambia de forma consistente con la
humedad del reactor, la forma correcta de refinarlo es repetir la calibración
multipunto en 2 condiciones de humedad ambiente distintas y comparar los A/B
resultantes — pero para eso ya se necesita más de una campaña de calibración,
que es trabajo de terreno, no algo que se resuelva solo con una constante.

## 2. MH-Z19Z (CO2) — ya es el sensor más preciso de los cuatro

Es NDIR (mide por absorción infrarroja), no depende de una curva Rs/R0 como el
MQ-4, así que su precisión de fábrica (±(50 ppm + 5% de la lectura)) ya es
buena. Lo único que vale la pena calibrar:

- **Zero point**: ya está implementado (`calibrate_co2_zero`), requiere 20
  minutos en aire fresco real (400 ppm aprox.) antes de aceptar el comando.
  Es la calibración más importante para no lecturas "clavadas" en 5000.
- **Span (opcional)**: si tenés gas patrón de CO2 a concentración conocida,
  el chip soporta el comando 0x88 de calibración de span. No lo agregué al
  firmware porque necesitás el patrón de gas para que tenga sentido — si lo
  conseguís, avisame y lo implemento con el mismo patrón de comandos que ya
  usa `calibrateMHZ19Zero()`.

## 3. DHT11 — limitación de hardware, no de software

El DHT11 tiene ±2°C y ±5% RH de precisión *de fábrica*, con resolución entera
(no reporta decimales reales, redondea internamente). Ningún ajuste de
software mejora la resolución; el offset solo corrige el **sesgo sistemático**
(que la lectura esté siempre X grados por encima o por debajo de la realidad).

Agregué el panel de "Ajuste DHT11" al dashboard (el comando
`set_dht_offset` ya existía en el firmware pero no tenía botón). Para
calibrarlo bien:

1. Poné un termómetro/higrómetro de referencia (aunque sea uno de mercurio o
   un higrómetro digital económico ya verificado) al lado del DHT11, en el
   mismo punto del ambiente.
2. Esperá a que ambos se estabilicen (10+ minutos).
3. Offset = referencia − lectura DHT11. Cargalo en el panel.

Si necesitás más precisión que ±2°C/±5%RH de forma real (no solo corregir
sesgo), la única solución es cambiar el sensor por un SHT31 o SHT21 — son
pin-compatibles en lógica I2C pero no en el mismo conector que usás hoy, así
que sería un cambio de hardware, no de calibración.

## 4. JSN-SR04T / AJ-SR04M (ultrasonido) — ya tenés 2 grados de libertad

El firmware ya soporta `offset` y `scale` (`set_distance_calibration`), que es
matemáticamente una calibración de 2 puntos (recta `dist_real = dist_medida *
scale + offset`). Para que el ajuste sea exacto y no a ojo:

1. Medí con cinta métrica dos distancias reales bien separadas dentro del
   rango de trabajo del reactor, por ejemplo 30 cm y 180 cm.
2. Anotá lo que reporta el sensor en cada una (`dist_raw_cm` en la telemetría,
   que ya vas a ver ahora en el JSON — es la distancia sin offset/escala).
3. Resolvé el sistema:
   ```
   scale  = (real_2 - real_1) / (medida_2 - medida_1)
   offset = real_1 - medida_1 * scale
   ```
4. Cargá esos dos valores en el panel "Calibración Reactor" → offset/escala.

Ya se compensa la velocidad del sonido con la temperatura real del DHT11
(`readUltrasonic()` usa `331.3 + 0.606·T`), así que la fuente de error que
queda es geométrica (ángulo del sensor, superficie no perfectamente plana del
biogás/líquido), y eso solo se corrige con el offset/escala de arriba, no con
más filtrado de software.

## Resumen de comandos nuevos

| Comando | Qué hace |
|---|---|
| `{"cmd":"add_calibration_point","ppm":N}` | Captura un punto de calibración de curva MQ-4 a N PPM de referencia (~10s) |
| `{"cmd":"fit_calibration_curve"}` | Ajusta A/B por regresión con los puntos capturados (mínimo 2) |
| `{"cmd":"reset_calibration_points"}` | Borra los puntos sin tocar la curva activa |
| `{"cmd":"reset_curve"}` | Restaura la curva genérica del datasheet (A=1012.7, B=-2.786) |
| `{"cmd":"set_dht_offset","temp":X,"hum":Y}` | Ya existía en firmware, ahora tiene botón en el dashboard |

Todos se pueden mandar por el monitor serial de Arduino IDE (115200 baudios)
o desde el dashboard web nuevo.
