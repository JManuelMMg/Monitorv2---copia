@echo off
cd /d "c:\Users\jmedi\Documents\proyectos\Monitor\web"
echo Activando entorno virtual...
call venv\Scripts\activate.bat
echo Ejecutando servidor FastAPI en puerto 8000...
echo Abre http://localhost:8000 en tu navegador
python -m uvicorn main:app --host 0.0.0.0 --port 8000 --reload
pause
