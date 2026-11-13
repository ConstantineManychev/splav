$OutputEncoding = [System.Text.Encoding]::UTF8
[Console]::OutputEncoding = [System.Text.Encoding]::UTF8
chcp 65001 | Out-Null
$env:PYTHONUTF8 = "1"

$llvmBin = "C:\Program Files\LLVM\bin"
if ((Test-Path $llvmBin) -and ($env:Path -notlike "*$llvmBin*"))
{
    $env:Path += ";$llvmBin"
}

Write-Host "=== Шаг 1. Проверки рантайма v0.050.8 (регион, капсула, ожидатьПобитно) ===" -ForegroundColor Cyan
clang++ -O3 -std=c++20 runtime_checks.cpp -o runtime_checks.exe
if ($LASTEXITCODE -eq 0)
{
    .\runtime_checks.exe region freeze ieee
}

Write-Host "=== Шаг 2. Прокси набор: невстроенные вызовы, клоны по диапазону, сдвиг столбцов и ABI четырёх платформ (§5 п. 4) ===" -ForegroundColor Cyan
clang++ -O3 -std=c++20 -ffp-contract=off proxy_bench.cpp proxy_abi.cpp -o proxy_bench.exe
if ($LASTEXITCODE -eq 0)
{
    .\proxy_bench.exe
}
$python = Get-Command python -ErrorAction SilentlyContinue
if (-not $python)
{
    $python = Get-Command py -ErrorAction SilentlyContinue
}
if ($python)
{
    & $python.Source abi_check.py
}
else
{
    Write-Host "Python не найден: проверка ABI пропущена." -ForegroundColor Yellow
}

Write-Host "=== Шаг 3. Проверка §7.3 (слить, Поле, Окно.сдвиг, настроить) и §3.5 (+?, -?, *?) ===" -ForegroundColor Cyan
clang++ -O3 -mavx2 -std=c++20 stencil_fusion_test.cpp -o stencil_fusion_test.exe
if ($LASTEXITCODE -eq 0)
{
    .\stencil_fusion_test.exe
}

Write-Host "=== Шаг 4. Программный fma v0.050.10: soft_fma.glsl побитно против независимого эталона на ЦПУ (§7.2 п. 12) ===" -ForegroundColor Cyan
clang++ -O3 -std=c++20 -ffp-contract=off fma_check.cpp -o fma_check.exe
if ($LASTEXITCODE -eq 0)
{
    .\fma_check.exe
}

Write-Host "=== Шаг 5. Vulkan v0.050.11: типы памяти, пробы с программным fma, VK_KHR_shader_fma и отдельным fma(), 400 шагов (сортировка на ЦПУ и всё на ГПУ, причина расхождения сортировки), полный шаг по фазам, резидентный конвейер против ЦПУ (медиана 5 партий) ===" -ForegroundColor Cyan
$vulkanSdk = $env:VULKAN_SDK
if (-not $vulkanSdk)
{
    $sdkRoot = "C:\VulkanSDK"
    if (Test-Path $sdkRoot)
    {
        $latest = Get-ChildItem $sdkRoot | Sort-Object Name -Descending | Select-Object -First 1
        if ($latest)
        {
            $vulkanSdk = $latest.FullName
        }
    }
}

if ($vulkanSdk -and (Test-Path "$vulkanSdk\Bin\glslc.exe"))
{
    $glslc = "$vulkanSdk\Bin\glslc.exe"
    $shadersBuilt = $true
    foreach ($shader in @("update", "update_soft", "update_approx", "probe", "sort_keys", "sort_keys_soft", "probe_fma"))
    {
        & $glslc -O --target-env=vulkan1.2 "$shader.comp" -o "$shader.spv"
        if ($LASTEXITCODE -ne 0)
        {
            $shadersBuilt = $false
        }
        & $glslc -O --target-env=vulkan1.2 -DDENORM_PRESERVE "$shader.comp" -o "${shader}_preserve.spv"
        if ($LASTEXITCODE -ne 0)
        {
            $shadersBuilt = $false
        }
    }
    foreach ($shader in @("scan", "scan_reduce", "sort_scatter", "sort_rank", "sort_gather"))
    {
        & $glslc -O --target-env=vulkan1.2 "$shader.comp" -o "$shader.spv"
        if ($LASTEXITCODE -ne 0)
        {
            $shadersBuilt = $false
        }
    }
    foreach ($mode in @("", "_preserve"))
    {
        $flags = @("-DFMA_KHR")
        if ($mode)
        {
            $flags += "-DDENORM_PRESERVE"
        }
        & $glslc --target-env=vulkan1.2 @flags probe_fma.comp -o "probe_fma_khr$mode.spv" 2>$null
        if ($LASTEXITCODE -ne 0)
        {
            Remove-Item "probe_fma_khr$mode.spv" -ErrorAction SilentlyContinue
            Write-Host "probe_fma_khr$mode.spv не собран: glslc не знает SPV_KHR_fma, путь OpFmaKHR пропущен." -ForegroundColor Yellow
        }
    }
    if ($shadersBuilt)
    {
        clang++ -O3 -std=c++20 -ffp-contract=off gpu_vulkan_test.cpp -I"$vulkanSdk\Include" -L"$vulkanSdk\Lib" -lvulkan-1 -o gpu_vulkan_test.exe
        if ($LASTEXITCODE -eq 0)
        {
            .\gpu_vulkan_test.exe
        }
    }
}
else
{
    Write-Host "Vulkan SDK не найден." -ForegroundColor Yellow
}

Write-Host "=== Шаг 6. Проверка CUDA (NVIDIA GeForce RTX 4070 Laptop, GDDR6) ===" -ForegroundColor Cyan
.\run_cuda.ps1
