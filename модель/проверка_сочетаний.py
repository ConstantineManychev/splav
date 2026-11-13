import pathlib
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))

from проверить import проверить_файл

КАТАЛОГ = pathlib.Path(sys.argv[1]) if len(sys.argv) > 1 else pathlib.Path(__file__).resolve().parent.parent / "корпус" / "злые-сочетания"


def главная():
    тесты = sorted(КАТАЛОГ.glob("*.спл"))
    прошло = 0
    ошибок = 0
    for путь in тесты:
        ждём = путь.with_suffix(".диаг").read_text(encoding="utf-8").splitlines()
        получено, неподдержано = проверить_файл(путь)
        ошибок += sum(1 for строка in ждём if " ошибка " in строка)
        if получено == ждём and not неподдержано:
            прошло += 1
            continue
        print(f"✗ {путь.name}")
        for строка in ждём:
            if строка not in получено:
                print(f"    ждали: {строка}")
        for строка in получено:
            if строка not in ждём:
                print(f"    выдано: {строка}")
        for модуль, номер, что in неподдержано:
            print(f"    неподдержано {модуль.имя}:{номер}: {что}")
    print(f"сочетания злых примеров: {прошло} из {len(тесты)} программ, ожидаемых ошибок {ошибок}")
    return прошло == len(тесты)


if __name__ == "__main__":
    sys.exit(0 if главная() else 1)
