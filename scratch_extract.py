import json
import os

transcript_path = r"C:\Users\jmedi\.gemini\antigravity-ide\brain\b391bab5-caea-4a83-9c8d-064b36fd081d\.system_generated\logs\transcript.jsonl"
target_ino_path = r"C:\Users\jmedi\OneDrive\Documents\proyectos\Monitor\arduino\mgas\mgas.ino"

found_content = None
with open(transcript_path, "r", encoding="utf-8") as f:
    for line in f:
        try:
            data = json.loads(line)
            if "tool_calls" in data:
                for tc in data["tool_calls"]:
                    if tc.get("name") == "write_to_file":
                        args = tc.get("args", {})
                        if isinstance(args, str):
                            args = json.loads(args)
                        if "mgas.ino" in args.get("TargetFile", ""):
                            found_content = args.get("CodeContent", "")
        except Exception as e:
            print("Error parsing line:", e)

if found_content:
    print("Found content length:", len(found_content))
    # If the content is double escaped or stringified JSON inside CodeContent,
    # let's make sure it's decoded properly.
    if found_content.startswith('"') and found_content.endswith('"'):
        try:
            found_content = json.loads(found_content)
        except:
            pass
    with open(target_ino_path, "w", encoding="utf-8") as out:
        out.write(found_content)
    print("Successfully wrote to mgas.ino!")
else:
    print("Could not find mgas.ino content in logs.")
