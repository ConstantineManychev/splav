#!/usr/bin/env bash
set -u
cd "$(dirname "$0")"

CXX="${CXX:-}"
if [ -z "$CXX" ]
then
    if command -v clang++ > /dev/null
    then
        CXX=clang++
    else
        CXX=g++
    fi
fi

echo "=== Шаг 1. Проверки рантайма v0.050.8 (регион, капсула, ожидатьПобитно) ==="
"$CXX" -O3 -std=c++20 -pthread runtime_checks.cpp -o runtime_checks && ./runtime_checks region freeze ieee

echo "=== Шаг 2. Прокси набор: невстроенные вызовы, клоны по диапазону, сдвиг столбцов и ABI четырёх платформ (§5 п. 4) ==="
"$CXX" -O3 -std=c++20 -ffp-contract=off proxy_bench.cpp proxy_abi.cpp -o proxy_bench && ./proxy_bench
if command -v python3 > /dev/null
then
    python3 abi_check.py
fi

echo "=== Шаг 3. Проверка §7.3 (слить, Поле, Окно.сдвиг, настроить) и §3.5 (+?, -?, *?) ==="
"$CXX" -O3 -mavx2 -std=c++20 stencil_fusion_test.cpp -o stencil_fusion_test && ./stencil_fusion_test

echo "=== Шаг 4. Программный fma v0.050.10: soft_fma.glsl побитно против независимого эталона на ЦПУ (§7.2 п. 12) ==="
"$CXX" -O3 -std=c++20 -ffp-contract=off -pthread fma_check.cpp -o fma_check && ./fma_check

echo "=== Шаг 5. Vulkan v0.050.11: типы памяти, пробы с программным fma, VK_KHR_shader_fma и отдельным fma(), 400 шагов (сортировка на ЦПУ и всё на ГПУ, причина расхождения сортировки), полный шаг по фазам, резидентный конвейер против ЦПУ (медиана 5 партий) ==="
if command -v glslc > /dev/null
then
    COMPILE=(glslc -O --target-env=vulkan1.2)
elif command -v glslangValidator > /dev/null
then
    COMPILE=(glslangValidator -V --target-env vulkan1.2)
else
    COMPILE=()
fi
if [ "${#COMPILE[@]}" -gt 0 ]
then
    built=1
    for shader in update update_soft update_approx probe sort_keys sort_keys_soft probe_fma
    do
        "${COMPILE[@]}" "$shader.comp" -o "$shader.spv" > /dev/null || built=0
        "${COMPILE[@]}" -DDENORM_PRESERVE "$shader.comp" -o "${shader}_preserve.spv" > /dev/null || built=0
    done
    for shader in scan scan_reduce sort_scatter sort_rank sort_gather
    do
        "${COMPILE[@]}" "$shader.comp" -o "$shader.spv" > /dev/null || built=0
    done
    for mode in "" _preserve
    do
        flags=(-DFMA_KHR)
        if [ -n "$mode" ]
        then
            flags+=(-DDENORM_PRESERVE)
        fi
        if [ "${COMPILE[0]}" = glslc ]
        then
            glslc --target-env=vulkan1.2 "${flags[@]}" probe_fma.comp -o "probe_fma_khr$mode.spv" > /dev/null 2>&1 || rm -f "probe_fma_khr$mode.spv"
        else
            "${COMPILE[@]}" "${flags[@]}" probe_fma.comp -o "probe_fma_khr$mode.spv" > /dev/null 2>&1 || rm -f "probe_fma_khr$mode.spv"
        fi
    done
    if [ "$built" = 1 ]
    then
        "$CXX" -O3 -std=c++20 -ffp-contract=off -pthread gpu_vulkan_test.cpp -lvulkan -o gpu_vulkan_test && ./gpu_vulkan_test
    fi
else
    echo "Нет glslc и glslangValidator: шаг пропущен"
fi

echo "=== Шаг 6. Эталон GL ES v0.050.11: пробы с программным fma, 400 шагов с аппаратными делением и корнем и с доводкой, контроль, замеры модели выбора (ЦПУ — лучший из скаляра и SPMD) ==="
if g++ -O2 -std=c++20 -ffp-contract=off гпу.cpp -o гпу -ldl -lpthread
then
    ./гпу сравнить
    ./гпу перестановка
    ./гпу замеры
fi
