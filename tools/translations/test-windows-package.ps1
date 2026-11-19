# Launch each language from an isolated portable profile and read the real first-run dialog.
param([Parameter(Mandatory)][string]$PackageDir)
$ErrorActionPreference = "Stop"
Add-Type -AssemblyName UIAutomationClient
Add-Type -AssemblyName UIAutomationTypes

$preview = Join-Path $env:RUNNER_TEMP "fritchii-language-smoke"
Copy-Item $PackageDir $preview -Recurse
$config = Join-Path $preview "user/config"
New-Item -ItemType Directory -Force $config | Out-Null
$env:FRITCHII_SETTINGS_PROMPT = "ask"
$env:QT_ACCESSIBILITY = "1"
$env:QT_PLUGIN_PATH = $null
$env:PATH = ($env:PATH -split ';' | Where-Object { $_ -notmatch '[\\/]Qt[\\/]' }) -join ';'

foreach ($test in @(
    @{ Locale = "pt_BR"; Title = "Usar as configurações recomendadas?" },
    @{ Locale = "en"; Title = "Use the recommended settings?" }
)) {
    @("[UI]", "Paths\language=$($test.Locale)", "Paths\language\default=false") |
        Set-Content (Join-Path $config "qt-config.ini") -Encoding utf8NoBOM
    $process = Start-Process -FilePath (Join-Path $preview "fritchii.exe") -WorkingDirectory $preview -PassThru
    try {
        $byProcess = [System.Windows.Automation.PropertyCondition]::new(
            [System.Windows.Automation.AutomationElement]::ProcessIdProperty, $process.Id)
        $byTitle = [System.Windows.Automation.PropertyCondition]::new(
            [System.Windows.Automation.AutomationElement]::NameProperty, $test.Title)
        $desktop = [System.Windows.Automation.AutomationElement]::RootElement
        $deadline = [DateTime]::UtcNow.AddSeconds(60)
        $found = $false
        do {
            if ($process.HasExited) { throw "Fritchii exited with code $($process.ExitCode)." }
            $windows = $desktop.FindAll([System.Windows.Automation.TreeScope]::Children, $byProcess)
            foreach ($window in $windows) {
                try {
                    $label = $window.FindFirst([System.Windows.Automation.TreeScope]::Subtree, $byTitle)
                    if ($null -ne $label) { $found = $true; break }
                } catch [System.Windows.Automation.ElementNotAvailableException] {
                    # Startup windows can close while accessibility is inspecting them.
                }
            }
            if (-not $found) { Start-Sleep -Milliseconds 500 }
        } until ($found -or [DateTime]::UtcNow -ge $deadline)
        if (-not $found) { throw "The packaged frontend did not display '$($test.Title)'." }
        Write-Host "$($test.Locale) - $($test.Title)"
    } finally {
        if (-not $process.HasExited) { $process.Kill(); $process.WaitForExit() }
        $process.Dispose()
    }
}
