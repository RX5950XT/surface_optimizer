import subprocess
import re

objdump_path = r"C:\msys64\ucrt64\bin\objdump.exe"
exe_path = r"C:\RX5950XT\terminal\surface_optimizer\surface_optimizer.exe"

res = subprocess.run([objdump_path, "-p", exe_path], capture_output=True, text=True)
lines = res.stdout.splitlines()

print("=== ALL IMPORTED WIN32 SYMBOLS ===")
keywords = ["Service", "Mutex", "Token", "Wait", "Power", "Event", "Hook", "Privilege", "Security"]
for line in lines:
    for kw in keywords:
        if kw.lower() in line.lower():
            print(line.strip())
            break
