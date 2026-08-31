#!/usr/bin/env python3
"""
Test script para simular lecturas del ESP32 y verificar integración con FastAPI
Uso: python test_esp32_sim.py
"""

import json
import time
import random
import requests
from datetime import datetime

# Configuración
BACKEND_URL = "http://localhost:8000/api/readings"
SIMULATION_TIME = 60  # segundos

class ESP32Simulator:
    def __init__(self):
        self.temp = 22.0
        self.hum = 50.0
        self.ppm = 100.0
        self.co2_ppm = 800.0
        self.dist = 50.0
        self.mq_r0 = 14800.0
        self.uptime = 0
        self.iteration = 0
    
    def simulate_readings(self):
        """Genera lecturas simuladas realistas"""
        # Temperatura fluctúa lentamente ±2°C
        self.temp += random.uniform(-0.5, 0.5)
        self.temp = max(15.0, min(35.0, self.temp))
        
        # Humedad fluctúa ±3%
        self.hum += random.uniform(-1.0, 1.0)
        self.hum = max(20.0, min(80.0, self.hum))
        
        # PPM fluctúa alrededor de 100-200 (gas residual)
        if self.iteration % 5 == 0:  # Ocasional spike
            self.ppm = random.uniform(200, 300)
        else:
            self.ppm = 100 + random.uniform(-20, 20)
        self.ppm = max(0.0, self.ppm)
        
        # Distancia/Nivel: fluctúa 48-52 cm
        self.dist = 50.0 + random.uniform(-2, 2)

        # CO2 del biodigestor / ambiente de prueba
        self.co2_ppm += random.uniform(-25, 25)
        self.co2_ppm = max(350.0, min(5000.0, self.co2_ppm))
        
        # Uptime incrementa cada segundo
        self.uptime += 1
        self.iteration += 1
        
        return {
            "temp": round(self.temp, 1),
            "hum": round(self.hum, 1),
            "ppm": round(self.ppm, 1),
            "co2_ppm": round(self.co2_ppm, 0),
            "dist": round(self.dist, 1),
            "mq_ready": True,
            "mq_r0": self.mq_r0,
            "wifi": False,
            "rssi": 0,
            "uptime": self.uptime
        }
    
    def send_reading(self, data):
        """Envía lectura al backend FastAPI"""
        try:
            response = requests.post(
                BACKEND_URL,
                json=data,
                timeout=2
            )
            if response.status_code == 200:
                return True, "OK"
            else:
                return False, f"HTTP {response.status_code}"
        except requests.exceptions.ConnectionError:
            return False, "Conexión rechazada (¿FastAPI corriendo?)"
        except requests.exceptions.Timeout:
            return False, "Timeout"
        except Exception as e:
            return False, str(e)
    
    def run(self, duration=SIMULATION_TIME):
        """Ejecuta la simulación por el tiempo especificado"""
        print(f"\n{'='*70}")
        print(f"ESP32 SIMULATOR - Imitando lecturas del hardware")
        print(f"{'='*70}")
        print(f"Backend: {BACKEND_URL}")
        print(f"Duración: {duration} segundos")
        print(f"{'='*70}\n")
        
        start_time = time.time()
        success_count = 0
        failure_count = 0
        
        try:
            while time.time() - start_time < duration:
                data = self.simulate_readings()
                success, msg = self.send_reading(data)
                
                status = "OK" if success else "ERROR"
                color_temp = "\033[31m" if data["temp"] > 32 else "\033[32m"
                color_ppm = "\033[31m" if data["ppm"] > 500 else "\033[32m"
                color_reset = "\033[0m"
                
                print(f"{status} [{data['uptime']}s] "
                      f"Temp: {color_temp}{data['temp']:.1f}°C{color_reset} | "
                      f"Hum: {data['hum']:.1f}% | "
                      f"PPM: {color_ppm}{data['ppm']:.1f}{color_reset} | "
                      f"Dist: {data['dist']:.1f}cm | {msg}")
                
                if success:
                    success_count += 1
                else:
                    failure_count += 1
                
                time.sleep(1)
        
        except KeyboardInterrupt:
            print("\n\n[INTERRUPCIÓN] Simulación detenida por usuario")
        
        print(f"\n{'='*70}")
        print(f"RESULTADOS FINALES")
        print(f"{'='*70}")
        print(f"Total enviado: {success_count + failure_count} lecturas")
        print(f"Exitosas: {success_count}")
        print(f"Fallidas: {failure_count}")
        if success_count > 0:
            print(f"Tasa de éxito: {(success_count*100/(success_count+failure_count)):.1f}%")
        print(f"{'='*70}\n")
        
        return success_count, failure_count


if __name__ == "__main__":
    print("\nIniciando simulador de ESP32 en 3 segundos...")
    print("(Asegúrate de que FastAPI está corriendo en http://0.0.0.0:8000)")
    time.sleep(3)
    
    simulator = ESP32Simulator()
    success, failure = simulator.run()
    
    if success > 0:
        print("SIMULACION EXITOSA - El sistema esta funcionando correctamente\n")
    else:
        print("SIMULACION FALLO - Verifica que FastAPI esta en puerto 8000\n")
