Set-ExecutionPolicy Bypass -Scope Process -Force
$ENGINE = ".\ShashChess39.1-x86-64-bmi2.exe"
$LOGFILE = "benchmark_log.txt"
$ITERATIONS = 5
$TEMP_OUTPUT_STDOUT = "temp_output_stdout.txt"
$TEMP_OUTPUT_STDERR = "temp_output_stderr.txt"
$TEMP_OUTPUT = "temp_output.txt"

# Lista dei programmi da chiudere
$PROGRAMS = @("chrome", "Teams", "Skype", "MBAMService", "WhatsApp")

# Ottenere il piano di alimentazione attuale
$CURRENT_POWER_PLAN = (powercfg /GETACTIVESCHEME) -match "([a-fA-F0-9\-]{36})" 
if ($matches) {
    $CURRENT_POWER_PLAN = $matches[1]
    Write-Host "Piano energetico attuale salvato: $CURRENT_POWER_PLAN"
} else {
    Write-Host "❌ ERRORE: Impossibile determinare il piano energetico attuale."
    exit 1
}

#**Preparazione ambiente**
Write-Host "Preparazione ambiente..."
Write-Host "Chiudo i programmi pesanti..."
foreach ($program in $PROGRAMS) {
    $process = Get-Process -Name $program -ErrorAction SilentlyContinue
    if ($process) {
        Write-Host "Chiudo $program"
        Stop-Process -Name $program -Force -ErrorAction SilentlyContinue
    } else {
        Write-Host "$program non e' in esecuzione"
    }
}

Write-Host "Disattivo la rete..."
Disable-NetAdapter -Name "*" -Confirm:$false

Write-Host "Imposto massime prestazioni energetiche..."
powercfg /SETACTIVE SCHEME_MIN

# Pulizia log con UTF-8 senza BOM
$header = @"
=== Benchmark ShashChess $(Get-Date -Format "MM/dd/yyyy HH:mm:ss") ===
========================================
"@

[System.Text.Encoding]::UTF8.GetBytes($header) | Set-Content -Path $LOGFILE -Encoding Byte
Write-Host $header

$TOTAL_TIME = 0
$TOTAL_NODES = 0
$TOTAL_NODES_SEC = 0

for ($i = 1; $i -le $ITERATIONS; $i++) {
    Write-Host "`n--- Iterazione $i/$ITERATIONS ---"

    Remove-Item -Path $TEMP_OUTPUT_STDOUT, $TEMP_OUTPUT_STDERR, $TEMP_OUTPUT -ErrorAction Ignore

    Write-Host "Avvio ShashChess benchmark..."
    Start-Process -FilePath $ENGINE -ArgumentList "bench" -NoNewWindow `
        -RedirectStandardOutput $TEMP_OUTPUT_STDOUT -RedirectStandardError $TEMP_OUTPUT_STDERR -Wait

    Start-Sleep -Seconds 1

    Get-Content $TEMP_OUTPUT_STDOUT, $TEMP_OUTPUT_STDERR | Set-Content $TEMP_OUTPUT

    if (!(Test-Path $TEMP_OUTPUT)) {
        Write-Host "ERRORE: Il file temp_output.txt non e' stato creato!"
        continue
    }

    $BENCH_TIME = $null
    $NODES = $null
    $NODES_SEC = $null

    $lines = Get-Content $TEMP_OUTPUT
    foreach ($line in $lines) {
        if ($line -match "Total time \(ms\)\s*:\s*(\d+)") { $BENCH_TIME = $matches[1] }
        if ($line -match "Nodes searched\s*:\s*(\d+)") { $NODES = $matches[1] }
        if ($line -match "Nodes/second\s*:\s*(\d+)") { $NODES_SEC = $matches[1] }
    }

    if ($BENCH_TIME -and $NODES -and $NODES_SEC) {
        Write-Host "DEBUG: Time: $BENCH_TIME ms, Nodes: $NODES, Nodes/sec: $NODES_SEC"
    } else {
        Write-Host "ERRORE: Nessun dato di riepilogo trovato in temp_output.txt!"
        Get-Content $TEMP_OUTPUT | Select-Object -Last 20
        continue
    }

    $TOTAL_TIME += [int]$BENCH_TIME
    $TOTAL_NODES += [int]$NODES
    $TOTAL_NODES_SEC += [int]$NODES_SEC

    $logLine = "Time: $BENCH_TIME ms, Nodes: $NODES, Nodes/sec: $NODES_SEC"
    [System.Text.Encoding]::UTF8.GetBytes("$logLine`n") | Add-Content -Path $LOGFILE -Encoding Byte
}

if ($TOTAL_TIME -eq 0 -or $TOTAL_NODES -eq 0 -or $TOTAL_NODES_SEC -eq 0) {
    Write-Host "ERRORE: Tutti i dati sono nulli. Controlla il file di output!"
    Read-Host "Premere INVIO per uscire..."
    exit
}

$AVG_TIME = [math]::Round($TOTAL_TIME / $ITERATIONS)
$AVG_NODES = [math]::Round($TOTAL_NODES / $ITERATIONS)
$AVG_NODES_SEC = [math]::Round($TOTAL_NODES_SEC / $ITERATIONS)

$summary = @"
========================================
Media dei risultati su $ITERATIONS esecuzioni:
Average Total time (ms) : $AVG_TIME
Average Nodes searched  : $AVG_NODES
Average Nodes/second    : $AVG_NODES_SEC
"@

Write-Host "`n$summary"
[System.Text.Encoding]::UTF8.GetBytes("$summary") | Add-Content -Path $LOGFILE -Encoding Byte

#**Ripristino ambiente**
Write-Host "`n Ripristino ambiente..."

Write-Host "Riattivo la rete..."
Enable-NetAdapter -Name "*" -Confirm:$false

Write-Host "Ripristino il piano energetico originale..."
if ($CURRENT_POWER_PLAN) {
    powercfg /SETACTIVE $CURRENT_POWER_PLAN
} else {
    powercfg /SETACTIVE SCHEME_BALANCED
}

Write-Host "Pulizia file temporanei..."
Remove-Item -Path $TEMP_OUTPUT_STDOUT, $TEMP_OUTPUT_STDERR, $TEMP_OUTPUT -ErrorAction Ignore

Write-Host "Benchmark completato."
Read-Host "Premere INVIO per uscire..."
