import os

def generate():
    root_dir = None
    try:
        Import("env")
        root_dir = env["PROJECT_DIR"]
    except Exception:
        pass

    if not root_dir:
        if "__file__" in globals():
            root_dir = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
        else:
            root_dir = os.getcwd()

    root_conf = os.path.join(root_dir, "ssid_list.conf")
    main_conf = os.path.join(root_dir, "main", "ssid_list.conf")
    out_path = os.path.join(root_dir, "main", "embedded_config.h")

    target_conf = None
    # Smart sync: whichever file was edited more recently is the source of truth
    if os.path.exists(root_conf) and os.path.exists(main_conf):
        try:
            if os.path.getmtime(main_conf) > os.path.getmtime(root_conf):
                target_conf = main_conf
                with open(main_conf, "r", encoding="utf-8") as f:
                    content = f.read()
                with open(root_conf, "w", encoding="utf-8") as f:
                    f.write(content)
            else:
                target_conf = root_conf
                with open(root_conf, "r", encoding="utf-8") as f:
                    content = f.read()
                with open(main_conf, "w", encoding="utf-8") as f:
                    f.write(content)
        except Exception as e:
            target_conf = main_conf if os.path.exists(main_conf) else root_conf
    elif os.path.exists(main_conf):
        target_conf = main_conf
    elif os.path.exists(root_conf):
        target_conf = root_conf

    if not target_conf or not os.path.exists(target_conf):
        print("[CONFIG] Warning: No ssid_list.conf found!")
        content = ""
    else:
        with open(target_conf, "r", encoding="utf-8") as f:
            content = f.read()

    normalized = content.replace("\r\n", "\n").replace("\r", "\n")
    lines = normalized.split("\n")
    escaped_lines = []
    for line in lines:
        esc = line.replace("\\", "\\\\").replace('"', '\\"')
        escaped_lines.append(f'    "{esc}\\n"')

    c_content = "\n".join(escaped_lines)
    header_code = f"""// AUTO-GENERATED FROM ssid_list.conf - DO NOT EDIT DIRECTLY
#pragma once

static const char EMBEDDED_SSID_LIST_CONF[] =
{c_content};
"""

    if os.path.exists(out_path):
        with open(out_path, "r", encoding="utf-8") as f:
            if f.read() == header_code:
                return

    with open(out_path, "w", encoding="utf-8") as f:
        f.write(header_code)
    print(f"[CONFIG] Generated embedded_config.h from {os.path.basename(target_conf)}")

generate()
