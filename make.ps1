param(
    [ValidateSet(
        "help",
        "bootstrap",
        "clean",
        "build",
        "build-wheels",
        "check",
        "install-dev",
        "build-inplace",
        "test"
    )]
    [string]$Target = "help"
)

$ErrorActionPreference = "Stop"

$Root = Split-Path -Parent $MyInvocation.MyCommand.Path
$DistDir = Join-Path $Root "dist"
$Wheelhouse = Join-Path $Root "wheelhouse"
$VenvPython = Join-Path $Root ".venv\Scripts\python.exe"

if (Test-Path $VenvPython) {
    $Python = $VenvPython
} else {
    $Python = "python"
}

function Show-Help {
    Write-Host "Targets:"
    Write-Host "  .\make.ps1 bootstrap     Install local packaging tools"
    Write-Host "  .\make.ps1 clean         Remove build artifacts"
    Write-Host "  .\make.ps1 build         Build sdist and Windows wheel into dist\"
    Write-Host "  .\make.ps1 build-wheels  Build configured wheels with cibuildwheel"
    Write-Host "  .\make.ps1 check         Validate dist\* with twine"
    Write-Host "  .\make.ps1 install-dev   Install editable package with dev deps"
    Write-Host "  .\make.ps1 build-inplace Build the C extension into credish\"
    Write-Host "  .\make.ps1 test          Run tests"
    Write-Host ""
    Write-Host "Tip: create a venv first with: py -3.12 -m venv .venv"
}

function Invoke-Python {
    & $Python @args
}

function Remove-IfExists {
    param([string]$Path)

    if (Test-Path $Path) {
        Remove-Item -LiteralPath $Path -Recurse -Force
    }
}

function Invoke-Clean {
    Remove-IfExists (Join-Path $Root "build")
    Remove-IfExists $DistDir
    Remove-IfExists $Wheelhouse
    Get-ChildItem -Path $Root -Filter "*.egg-info" -Directory -Force | Remove-Item -Recurse -Force
    Get-ChildItem -Path (Join-Path $Root "credish") -Filter "*.pyd" -File -Force | Remove-Item -Force
    Get-ChildItem -Path (Join-Path $Root "credish") -Filter "*.so" -File -Force | Remove-Item -Force
    Get-ChildItem -Path $Root -Recurse -Directory -Filter "__pycache__" -Force | Remove-Item -Recurse -Force
}

Push-Location $Root
try {
    switch ($Target) {
        "help" {
            Show-Help
        }
        "bootstrap" {
            Invoke-Python -m pip install --upgrade pip build twine cibuildwheel pytest pytest-timeout setuptools wheel
        }
        "clean" {
            Invoke-Clean
        }
        "build" {
            Invoke-Clean
            Invoke-Python -m build
        }
        "build-wheels" {
            Invoke-Clean
            Invoke-Python -m cibuildwheel --output-dir $DistDir
        }
        "check" {
            Invoke-Python -m twine check (Join-Path $DistDir "*")
        }
        "install-dev" {
            Invoke-Python -m pip install -e ".[dev]"
        }
        "build-inplace" {
            Invoke-Python setup.py build_ext --inplace
        }
        "test" {
            Invoke-Python -m pytest tests
        }
    }
} finally {
    Pop-Location
}
