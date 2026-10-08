@echo off
cd /d "%~dp0web"
if not exist "venv\Scripts\python.exe" (
	echo No se encontro el entorno virtual en web\venv.
	pause
	exit /b 1
)
echo Ejecutando servidor FastAPI en puerto 8000...
echo Abre http://localhost:8000 en tu navegador
venv\Scripts\python.exe -m uvicorn main:app --host 0.0.0.0 --port 8000 --reload
pause
