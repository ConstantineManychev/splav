$OutputEncoding = [System.Text.Encoding]::UTF8
[Console]::OutputEncoding = [System.Text.Encoding]::UTF8
chcp 65001 | Out-Null

$cudaRoot = "C:\Program Files\NVIDIA GPU Computing Toolkit\CUDA"
if (Test-Path $cudaRoot)
{
    $latestCuda = Get-ChildItem $cudaRoot | Sort-Object Name -Descending | Select-Object -First 1
    if ($latestCuda)
    {
        $cudaBin = "$($latestCuda.FullName)\bin"
        if ($env:Path -notlike "*$cudaBin*")
        {
            $env:Path = "$cudaBin;$env:Path"
        }
    }
}

if (-not (Get-Command cl.exe -ErrorAction SilentlyContinue))
{
    $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    if (Test-Path $vswhere)
    {
        $vsPath = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
        if ($vsPath)
        {
            $vcvars = "$vsPath\VC\Auxiliary\Build\vcvars64.bat"
            if (Test-Path $vcvars)
            {
                $tempBat = [System.IO.Path]::GetTempFileName() + ".cmd"
                "@call `"$vcvars`" >nul && set" | Set-Content -Path $tempBat -Encoding Ascii
                cmd /c $tempBat | ForEach-Object {
                    if ($_ -match "^([^=]+)=(.*)$")
                    {
                        [System.Environment]::SetEnvironmentVariable($matches[1], $matches[2], "Process")
                    }
                }
                Remove-Item $tempBat -ErrorAction SilentlyContinue
            }
        }
    }
}

if (Get-Command nvcc -ErrorAction SilentlyContinue)
{
    nvcc -O3 -std=c++20 -arch=sm_89 --fmad=false --allow-unsupported-compiler -Xcompiler "/utf-8 /EHsc /wd4828" gpu_cuda_test.cu -o gpu_cuda_test.exe
    if ($LASTEXITCODE -eq 0)
    {
        .\gpu_cuda_test.exe
    }
}
