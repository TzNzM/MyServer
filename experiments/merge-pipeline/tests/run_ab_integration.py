from pathlib import Path
import hashlib
import json
import os
import re
import subprocess
import uuid

root = Path(__file__).resolve().parent
env = dict(os.environ)
env["PATH"] = "D:/Qt/5.14.2/mingw73_64/bin;D:/Qt/Tools/mingw730_64/bin;" + env["PATH"]
env["QT_QPA_PLATFORM"] = "offscreen"
env["QT_QPA_PLATFORM_PLUGIN_PATH"] = "D:/Qt/5.14.2/mingw73_64/plugins/platforms"
results = []
for mode in ("serial", "pipeline"):
    data = root / ("integration-" + mode + "-" + uuid.uuid4().hex[:8])
    assert not data.exists(), "Use a fresh test directory"
    proc = subprocess.run([str(root / "ui-check/release/ui_transfer_check.exe"), str(data)],
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
        env={**env, "MYSERVER_MERGE_MODE": mode}, timeout=80,
        creationflags=subprocess.CREATE_NO_WINDOW)
    (root / ("integration-" + mode + ".log")).write_bytes(proc.stdout)
    text = proc.stdout.decode("utf-8", errors="replace")
    lines = [line for line in text.splitlines() if line.startswith("[合并")]
    assert proc.returncode == 0 and "ALL_OK" in text, text
    assert len(lines) == 2, lines
    transfers = []
    for index in range(2):
        source = data / f"input_{index}.bin"
        target = data / "received" / source.name
        expected = hashlib.sha256(source.read_bytes()).hexdigest()
        actual = hashlib.sha256(target.read_bytes()).hexdigest()
        assert expected == actual
        line = next(line for line in lines if f"文件={source.name}，" in line)
        assert "结果=完成，" in line
        assert f"已合并={source.stat().st_size}/{source.stat().st_size} 字节" in line
        fields = {key: float(value) for key, value in re.findall(
            r"(总计|等锁|读取|写入|SHA-256（并行，不计入相加）|SHA-256|commit|改名|清理|其他|等空闲缓冲|等摘要收尾)=([0-9.]+) ms", line)}
        if mode == "serial":
            assert line.startswith("[合并计时] 模式=串行，"), line
            stages = sum(value for key, value in fields.items() if key != "总计")
        else:
            assert line.startswith("[合并流水线计时] 模式=流水线，"), line
            assert "等空闲缓冲" in fields and "等摘要收尾" in fields
            assert "SHA-256（并行，不计入相加）" in fields
            stages = sum(value for key, value in fields.items() if key != "总计" and not key.startswith("SHA-256"))
        assert abs(fields["总计"] - stages) < 1.5, fields
        assert "%" not in line
        transfers.append(dict(bytes=source.stat().st_size, sha256=actual, passed=True, log=line))
    results.append(dict(mode=mode, passed=True, transfers=transfers))
    print("PASS", mode, "two real MainWidget TCP transfers and timing log", flush=True)
(root / "integration-results.json").write_text(json.dumps(results, ensure_ascii=False, indent=2), encoding="utf-8")
print("RESULT 4/4 real transfers", flush=True)
