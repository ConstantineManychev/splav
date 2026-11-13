import re
import shutil
import subprocess
import sys
from pathlib import Path

TARGETS = [
    ("x86_64-linux-gnu", "x86-64 System V: Linux, Steam Deck"),
    ("x86_64-pc-windows-msvc", "x86-64 Windows"),
    ("aarch64-linux-gnu", "AArch64 Linux"),
    ("arm64-apple-macos", "AArch64 macOS"),
]

PAIRS = [
    ("stepThreeWords", "runThreeWords"),
    ("stepProxyValue", "runProxyValue"),
    ("stepProxyReference", "runProxyReference"),
    ("stepProxyValueInternal", "runProxyValueInternal"),
    ("stepAos", "runAos"),
    ("stepEight", "runEight"),
    ("stepRange", "runRange"),
]

FLAGS = ["-O3", "-std=c++20", "-ffp-contract=off", "-fno-exceptions", "-S"]


def run_clang(compiler, source, target, emit_ir):
    command = [compiler, *FLAGS, "--target=" + target, str(source), "-o", "-"]
    if emit_ir:
        command.insert(len(FLAGS) + 1, "-emit-llvm")
    completed = subprocess.run(command, capture_output=True, encoding="utf-8", errors="replace", check=True)
    return completed.stdout


def named(symbol, name):
    itanium = re.search(r"(^|[^0-9])" + str(len(name)) + name, symbol) is not None
    microsoft = symbol.startswith("?" + name + "@")
    return itanium or microsoft


def ir_functions(ir):
    functions = {}
    lines = ir.splitlines()
    index = 0
    while index < len(lines):
        line = lines[index]
        if line.startswith("define "):
            symbol = re.search(r"@(\"[^\"]+\"|[^\s(]+)\(", line).group(1).strip('"')
            opening = line.index("(", line.index("@"))
            depth = 0
            closing = opening
            for position in range(opening, len(line)):
                if line[position] == "(":
                    depth += 1
                elif line[position] == ")":
                    depth -= 1
                    if depth == 0:
                        closing = position
                        break
            body = []
            index += 1
            while index < len(lines) and lines[index] != "}":
                body.append(lines[index])
                index += 1
            functions[symbol] = (line[:opening], line[opening + 1:closing], "\n".join(body))
        index += 1
    return functions


def find(functions, name):
    for symbol, value in functions.items():
        if named(symbol, name):
            return symbol, value
    return None, None


def parameter_types(parameters):
    kinds = []
    for parameter in parameters.split(", "):
        words = parameter.split()
        if not words:
            continue
        byval = re.search(r"byval\(%struct\.(\w+)\)", parameter)
        kinds.append("ptr byval " + byval.group(1) if byval else words[0])
    return kinds


WORDS = {3: "три слова в регистрах", 4: "четыре слова в регистрах"}


def passing(kinds):
    if any("byval" in kind for kind in kinds):
        return "копия на стеке аргументов"
    words = 0
    while words < len(kinds) and kinds[words] in ("ptr", "i64"):
        words += 1
    if words in WORDS and kinds[0] == "ptr":
        return WORDS[words]
    if kinds and kinds[0] == "ptr":
        return "адрес"
    return "-"


def asm_function(asm, symbol):
    lines = asm.splitlines()
    start = None
    for position, line in enumerate(lines):
        label = line.split(":")[0].strip('"')
        if not line.startswith((" ", "\t", ".")) and ":" in line and label.lstrip("_") == symbol.lstrip("_"):
            start = position
            break
    if start is None:
        return []
    body = []
    for line in lines[start + 1:]:
        stripped = line.strip()
        if ".cfi_endproc" in stripped or ".seh_endproc" in stripped:
            break
        if not stripped or stripped.startswith((".", "#", ";", "//")) or stripped.endswith(":"):
            continue
        body.append(stripped)
    return body


def vectorized(body):
    return re.search(r"<\d+ x float>", body) is not None


def address_comparisons(body):
    return len(re.findall(r"icmp (?:ult|ugt|ule|uge) ptr", body))


def body_summary(body):
    vectors = "векторное" if vectorized(body) else "скалярное"
    comparisons = address_comparisons(body)
    checks = f"проверка перекрытия столбцов: {comparisons} сравнений адресов" if comparisons else "без проверки перекрытия"
    return vectors + ", " + checks


def stack_traffic(instructions):
    pattern = re.compile(r"\(%rsp\)|\(%rbp\)|\[sp|\[x29")
    return sum(1 for text in instructions if pattern.search(text))


def main():
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    compiler = shutil.which("clang++")
    if compiler is None:
        print("нет clang++: проверка ABI пропущена")
        return 0
    source = Path(__file__).with_name("proxy_abi.cpp")
    print("=== ABI прокси набор (§5 п. 4): как clang -O3 передаёт 24-байтный прокси между невстроенными функциями ===")
    print("    функция: параметры в LLVM IR и способ передачи; вызывающая функция с циклом: alloca и store в IR, обращения к стеку в её машинном коде; тело функции")
    failures = 0
    for target, title in TARGETS:
        try:
            ir = run_clang(compiler, source, target, True)
            asm = run_clang(compiler, source, target, False)
        except subprocess.CalledProcessError as error:
            lines = error.stderr.strip().splitlines()
            print(f"{target}: clang не собрал ({lines[-1] if lines else 'ошибка'})")
            failures += 1
            continue
        functions = ir_functions(ir)
        print(f"{target} ({title}):")
        _, inline_value = find(functions, "runInline")
        if inline_value is not None:
            print(f"  {'runInline':24s} цикл со встроенным шагом: {body_summary(inline_value[2])}")
        for callee, caller in PAIRS:
            callee_symbol, callee_value = find(functions, callee)
            caller_symbol, caller_value = find(functions, caller)
            if callee_value is None:
                print(f"  {callee:24s} встроена")
                continue
            header, parameters, body = callee_value
            kinds = parameter_types(parameters)
            convention = "fastcc " if "fastcc" in header else ""
            allocas = len(re.findall(r"= alloca ", caller_value[2])) if caller_value else 0
            stores = len(re.findall(r"^\s*store ", caller_value[2], re.M)) if caller_value else 0
            traffic = stack_traffic(asm_function(asm, caller_symbol)) if caller_symbol else 0
            signature = convention + "(" + ", ".join(kinds) + ")"
            print(f"  {callee:24s} {signature:52s} {passing(kinds):26s} вызывающая: alloca {allocas}, store {stores}, стек {traffic}; "
                  f"тело {body_summary(body)}")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
