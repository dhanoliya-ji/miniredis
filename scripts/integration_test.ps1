# End-to-end integration tests for MiniRedis.
#
# The C++ suite in tests/ covers the pieces in isolation. This script covers
# what only shows up when real processes talk over real sockets: crash
# recovery from the append-only file, a replica synchronising with a master,
# read-only enforcement, eviction under a memory limit, and transactions.
#
#   .\scripts\integration_test.ps1
#   .\scripts\integration_test.ps1 -BasePort 7400 -KeepLogs

param(
    [int]$BasePort = 7350,
    [switch]$KeepLogs
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot

$server = Join-Path $root 'bin/miniredis-server.exe'
$cli = Join-Path $root 'bin/miniredis-cli.exe'

foreach ($binary in @($server, $cli)) {
    if (-not (Test-Path $binary)) {
        Write-Host "Missing $binary - run .\scripts\build.ps1 first" -ForegroundColor Red
        exit 1
    }
}

$workDir = Join-Path ([System.IO.Path]::GetTempPath()) "miniredis-integration-$PID"
New-Item -ItemType Directory -Force -Path $workDir | Out-Null

$script:passed = 0
$script:failed = 0
$script:failures = @()
$script:processes = @()

function Start-Node {
    param([string]$Name, [int]$Port, [string[]]$ExtraArgs = @())

    $dataDir = Join-Path $workDir $Name
    New-Item -ItemType Directory -Force -Path $dataDir | Out-Null

    # A bare `--save` with no value disables scheduled snapshots. It has to go
    # last, because Start-Process silently drops an empty-string element from
    # ArgumentList, which would otherwise shift every following argument.
    $arguments = @('--port', $Port, '--dir', $dataDir, '--nodeid', $Name) + $ExtraArgs + @('--save')
    $logPath = Join-Path $workDir "$Name.log"

    $process = Start-Process -FilePath $server -ArgumentList $arguments -PassThru `
        -RedirectStandardOutput $logPath -RedirectStandardError "$logPath.err" -WindowStyle Hidden
    $script:processes += $process

    # Wait for the port to answer rather than sleeping a fixed amount: a fixed
    # sleep is either slow or flaky, and usually both on a loaded machine.
    for ($attempt = 0; $attempt -lt 100; $attempt++) {
        Start-Sleep -Milliseconds 100
        try {
            $probe = Invoke-Cmd $Port @('PING')
            if ($probe -match 'PONG') { return $process }
        } catch { }
    }

    Write-Host "  node $Name did not come up on port $Port" -ForegroundColor Red
    if (Test-Path $logPath) { Get-Content $logPath -Tail 20 | ForEach-Object { Write-Host "    $_" } }
    throw "node $Name failed to start"
}

function Stop-Node {
    param($Process)
    if ($null -eq $Process -or $Process.HasExited) { return }
    try { $Process.Kill(); [void]$Process.WaitForExit(3000) } catch { }
}

function Invoke-Cmd {
    param([int]$Port, [string[]]$Arguments)

    # Windows PowerShell wraps a native program's stderr in ErrorRecords, which
    # under $ErrorActionPreference = 'Stop' aborts the script the first time the
    # server legitimately returns an error reply. Since half these tests exist
    # precisely to check error replies, stderr is captured through a file and
    # the preference is relaxed for the duration of the call.
    $previous = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    $errorFile = [System.IO.Path]::GetTempFileName()
    try {
        $stdout = & $cli -p $Port @Arguments 2>$errorFile
        $stderr = Get-Content $errorFile -Raw -ErrorAction SilentlyContinue
        return (@($stdout) + @($stderr) | Out-String).Trim()
    }
    finally {
        $ErrorActionPreference = $previous
        Remove-Item $errorFile -Force -ErrorAction SilentlyContinue
    }
}

function Invoke-Script {
    param([int]$Port, [string]$Script)

    $previous = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    $errorFile = [System.IO.Path]::GetTempFileName()
    try {
        # Piping a string to a native program emits a UTF-8 BOM by default,
        # which the server would read as part of the first command name.
        $previousEncoding = $OutputEncoding
        $OutputEncoding = New-Object System.Text.UTF8Encoding $false
        $stdout = $Script | & $cli -p $Port 2>$errorFile
        $OutputEncoding = $previousEncoding
        $stderr = Get-Content $errorFile -Raw -ErrorAction SilentlyContinue
        return (@($stdout) + @($stderr) | Out-String)
    }
    finally {
        $ErrorActionPreference = $previous
        Remove-Item $errorFile -Force -ErrorAction SilentlyContinue
    }
}

function Assert-Match {
    param([string]$What, [string]$Actual, [string]$Expected)

    if ($Actual -match [regex]::Escape($Expected)) {
        Write-Host "  [ ok ] $What" -ForegroundColor Green
        $script:passed++
    } else {
        Write-Host "  [FAIL] $What" -ForegroundColor Red
        Write-Host "         expected to contain: $Expected" -ForegroundColor DarkGray
        Write-Host "         actual:              $Actual" -ForegroundColor DarkGray
        $script:failed++
        $script:failures += $What
    }
}

function Assert-Regex {
    param([string]$What, [string]$Actual, [string]$Pattern)

    # Assert-Match escapes its needle so a literal comparison cannot be derailed
    # by punctuation in the expected text. This is the variant for the handful
    # of checks that genuinely want a pattern.
    if ($Actual -match $Pattern) {
        Write-Host "  [ ok ] $What" -ForegroundColor Green
        $script:passed++
    } else {
        Write-Host "  [FAIL] $What" -ForegroundColor Red
        Write-Host "         expected to match: $Pattern" -ForegroundColor DarkGray
        Write-Host "         actual:            $Actual" -ForegroundColor DarkGray
        $script:failed++
        $script:failures += $What
    }
}

function Assert-NotMatch {
    param([string]$What, [string]$Actual, [string]$Unexpected)

    if ($Actual -notmatch [regex]::Escape($Unexpected)) {
        Write-Host "  [ ok ] $What" -ForegroundColor Green
        $script:passed++
    } else {
        Write-Host "  [FAIL] $What" -ForegroundColor Red
        Write-Host "         should not have contained: $Unexpected" -ForegroundColor DarkGray
        Write-Host "         actual:                    $Actual" -ForegroundColor DarkGray
        $script:failed++
        $script:failures += $What
    }
}

Write-Host "MiniRedis integration tests" -ForegroundColor Cyan
Write-Host "  working directory: $workDir`n"

try {
    # -----------------------------------------------------------------------
    Write-Host "Core data types" -ForegroundColor Yellow
    $port = $BasePort
    $node = Start-Node -Name 'core' -Port $port

    Assert-Match "PING answers PONG" (Invoke-Cmd $port @('PING')) 'PONG'
    Invoke-Cmd $port @('SET', 'greeting', 'hello') | Out-Null
    Assert-Match "SET then GET returns the value" (Invoke-Cmd $port @('GET', 'greeting')) 'hello'
    Assert-Match "GET of a missing key is nil" (Invoke-Cmd $port @('GET', 'nothing')) 'nil'

    Invoke-Cmd $port @('RPUSH', 'q', 'a', 'b', 'c') | Out-Null
    Assert-Match "LLEN counts list elements" (Invoke-Cmd $port @('LLEN', 'q')) '3'
    Assert-Match "LPOP removes from the head" (Invoke-Cmd $port @('LPOP', 'q')) 'a'

    Invoke-Cmd $port @('HSET', 'user', 'name', 'alice', 'age', '30') | Out-Null
    Assert-Match "HGET reads a field" (Invoke-Cmd $port @('HGET', 'user', 'name')) 'alice'

    Invoke-Cmd $port @('ZADD', 'board', '100', 'alice', '90', 'bob') | Out-Null
    Assert-Match "ZRANGE orders by score" (Invoke-Cmd $port @('ZRANGE', 'board', '0', '0')) 'bob'
    Assert-Match "ZSCORE reads a score" (Invoke-Cmd $port @('ZSCORE', 'board', 'alice')) '100'

    Assert-Match "wrong type is refused" (Invoke-Cmd $port @('LPUSH', 'greeting', 'x')) 'WRONGTYPE'
    Assert-Match "unknown command is refused" (Invoke-Cmd $port @('NOTACOMMAND')) 'unknown command'
    Assert-Match "wrong arity is refused" (Invoke-Cmd $port @('GET')) 'wrong number of arguments'

    # -----------------------------------------------------------------------
    Write-Host "`nExpiry" -ForegroundColor Yellow
    Invoke-Cmd $port @('SET', 'temp', 'v', 'PX', '300') | Out-Null
    Assert-Regex "TTL is reported before expiry" (Invoke-Cmd $port @('PTTL', 'temp')) '\(integer\) \d+'
    Start-Sleep -Milliseconds 600
    Assert-Match "the key is gone after its TTL" (Invoke-Cmd $port @('GET', 'temp')) 'nil'
    Assert-Match "TTL of a missing key is -2" (Invoke-Cmd $port @('TTL', 'temp')) '-2'

    Invoke-Cmd $port @('SET', 'keeper', 'v') | Out-Null
    Invoke-Cmd $port @('EXPIRE', 'keeper', '100') | Out-Null
    Invoke-Cmd $port @('PERSIST', 'keeper') | Out-Null
    Assert-Match "PERSIST clears the TTL" (Invoke-Cmd $port @('TTL', 'keeper')) '-1'

    # -----------------------------------------------------------------------
    Write-Host "`nTransactions" -ForegroundColor Yellow
    $multiOutput = Invoke-Script $port "MULTI`nSET tx 1`nINCR tx`nEXEC`nGET tx`nexit`n"
    Assert-Match "queued commands run on EXEC" $multiOutput '"2"'

    $abortOutput = Invoke-Script $port "MULTI`nNOSUCHCOMMAND x`nEXEC`nexit`n"
    Assert-Match "a bad command aborts the transaction" $abortOutput 'EXECABORT'

    Stop-Node $node

    # -----------------------------------------------------------------------
    Write-Host "`nCrash recovery from the append-only file" -ForegroundColor Yellow
    $port = $BasePort + 1
    $aofNode = Start-Node -Name 'aof' -Port $port -ExtraArgs @('--appendonly', 'yes', '--appendfsync', 'always')

    Invoke-Cmd $port @('SET', 'durable', 'survives') | Out-Null
    Invoke-Cmd $port @('RPUSH', 'items', 'x', 'y', 'z') | Out-Null
    Invoke-Cmd $port @('HSET', 'profile', 'city', 'pune') | Out-Null
    Invoke-Cmd $port @('ZADD', 'ranks', '5', 'first') | Out-Null
    Invoke-Cmd $port @('SET', 'expiring', 'v', 'EX', '3600') | Out-Null

    # Kill rather than shut down: this has to look like a crash, not a clean
    # exit, or the test proves nothing about recovery.
    Stop-Node $aofNode
    Start-Sleep -Milliseconds 300

    $aofNode = Start-Node -Name 'aof' -Port $port -ExtraArgs @('--appendonly', 'yes')
    Assert-Match "a string survives a crash" (Invoke-Cmd $port @('GET', 'durable')) 'survives'
    Assert-Match "a list survives a crash" (Invoke-Cmd $port @('LLEN', 'items')) '3'
    Assert-Match "a hash survives a crash" (Invoke-Cmd $port @('HGET', 'profile', 'city')) 'pune'
    Assert-Match "a sorted set survives a crash" (Invoke-Cmd $port @('ZSCORE', 'ranks', 'first')) '5'
    Assert-Regex "a TTL survives a crash" (Invoke-Cmd $port @('TTL', 'expiring')) '\(integer\) 3[0-9]{3}'

    Invoke-Cmd $port @('BGREWRITEAOF') | Out-Null
    Assert-Match "the dataset survives an AOF rewrite" (Invoke-Cmd $port @('GET', 'durable')) 'survives'
    Stop-Node $aofNode

    # -----------------------------------------------------------------------
    Write-Host "`nSnapshot persistence" -ForegroundColor Yellow
    $port = $BasePort + 2
    $rdbNode = Start-Node -Name 'rdb' -Port $port

    Invoke-Cmd $port @('SET', 'snapshotted', 'yes') | Out-Null
    Invoke-Cmd $port @('SAVE') | Out-Null
    Stop-Node $rdbNode
    Start-Sleep -Milliseconds 300

    $rdbNode = Start-Node -Name 'rdb' -Port $port
    Assert-Match "the snapshot reloads on startup" (Invoke-Cmd $port @('GET', 'snapshotted')) 'yes'
    Stop-Node $rdbNode

    # -----------------------------------------------------------------------
    Write-Host "`nReplication" -ForegroundColor Yellow
    $masterPort = $BasePort + 3
    $replicaPort = $BasePort + 4

    $master = Start-Node -Name 'master' -Port $masterPort
    Invoke-Cmd $masterPort @('SET', 'before-replica', 'existing') | Out-Null

    $replica = Start-Node -Name 'replica' -Port $replicaPort `
        -ExtraArgs @('--replicaof', '127.0.0.1', $masterPort)

    # Wait for the link rather than guessing how long a sync takes.
    $synced = $false
    for ($attempt = 0; $attempt -lt 60; $attempt++) {
        Start-Sleep -Milliseconds 200
        if ((Invoke-Cmd $replicaPort @('GET', 'before-replica')) -match 'existing') {
            $synced = $true
            break
        }
    }
    Assert-Match "the replica receives the existing dataset" ($(if ($synced) { 'synced' } else { 'not synced' })) 'synced'

    Invoke-Cmd $masterPort @('SET', 'after-replica', 'streamed') | Out-Null
    $streamed = $false
    for ($attempt = 0; $attempt -lt 60; $attempt++) {
        Start-Sleep -Milliseconds 100
        if ((Invoke-Cmd $replicaPort @('GET', 'after-replica')) -match 'streamed') {
            $streamed = $true
            break
        }
    }
    Assert-Match "later writes stream to the replica" ($(if ($streamed) { 'streamed' } else { 'not streamed' })) 'streamed'

    Assert-Match "the replica refuses writes" (Invoke-Cmd $replicaPort @('SET', 'nope', 'x')) 'READONLY'
    Assert-Match "the replica still serves reads" (Invoke-Cmd $replicaPort @('GET', 'after-replica')) 'streamed'
    Assert-Match "the master reports its replica" (Invoke-Cmd $masterPort @('INFO', 'replication')) 'connected_slaves:1'
    Assert-Match "the replica reports its role" (Invoke-Cmd $replicaPort @('INFO', 'replication')) 'role:slave'

    Invoke-Cmd $masterPort @('DEL', 'after-replica') | Out-Null
    $deleted = $false
    for ($attempt = 0; $attempt -lt 60; $attempt++) {
        Start-Sleep -Milliseconds 100
        if ((Invoke-Cmd $replicaPort @('GET', 'after-replica')) -match 'nil') {
            $deleted = $true
            break
        }
    }
    Assert-Match "deletes propagate to the replica" ($(if ($deleted) { 'deleted' } else { 'still present' })) 'deleted'

    Invoke-Cmd $replicaPort @('REPLICAOF', 'NO', 'ONE') | Out-Null
    Start-Sleep -Milliseconds 300
    Assert-Match "a promoted replica accepts writes" (Invoke-Cmd $replicaPort @('SET', 'promoted', 'yes')) 'OK'

    Stop-Node $replica
    Stop-Node $master

    # -----------------------------------------------------------------------
    Write-Host "`nEviction under a memory limit" -ForegroundColor Yellow
    $port = $BasePort + 5
    $evictNode = Start-Node -Name 'evict' -Port $port `
        -ExtraArgs @('--maxmemory', '2mb', '--maxmemory-policy', 'allkeys-lru')

    # Write well past the limit; the policy has to keep the server inside it.
    $filler = 'x' * 1024
    for ($i = 0; $i -lt 4000; $i++) {
        Invoke-Cmd $port @('SET', "fill:$i", $filler) | Out-Null
    }

    $info = Invoke-Cmd $port @('INFO', 'stats')
    Assert-Match "keys were evicted to stay under maxmemory" $info 'evicted_keys'
    $evicted = [int]([regex]::Match($info, 'evicted_keys:(\d+)').Groups[1].Value)
    Assert-Match "the eviction count is non-zero" ($(if ($evicted -gt 0) { 'evicted' } else { 'none' })) 'evicted'
    Assert-Match "writes still succeed while evicting" (Invoke-Cmd $port @('SET', 'still', 'working')) 'OK'

    Stop-Node $evictNode

    # -----------------------------------------------------------------------
    Write-Host "`nNo eviction means writes are refused" -ForegroundColor Yellow
    $port = $BasePort + 6
    $oomNode = Start-Node -Name 'oom' -Port $port `
        -ExtraArgs @('--maxmemory', '1mb', '--maxmemory-policy', 'noeviction')

    $sawOom = $false
    for ($i = 0; $i -lt 4000; $i++) {
        $result = Invoke-Cmd $port @('SET', "fill:$i", $filler)
        if ($result -match 'OOM') { $sawOom = $true; break }
    }
    Assert-Match "noeviction refuses writes with OOM" ($(if ($sawOom) { 'OOM' } else { 'no OOM seen' })) 'OOM'
    Assert-Regex "reads still work when out of memory" (Invoke-Cmd $port @('DBSIZE')) '\(integer\) \d+'

    Stop-Node $oomNode

    # -----------------------------------------------------------------------
    Write-Host "`nSQL layer" -ForegroundColor Yellow
    $port = $BasePort + 7
    $sqlNode = Start-Node -Name 'sql' -Port $port

    Invoke-Cmd $port @('SQL', "INSERT INTO kv VALUES ('user:1', 'alice')") | Out-Null
    Assert-Match "SQL INSERT stores a value" (Invoke-Cmd $port @('GET', 'user:1')) 'alice'
    Assert-Match "SQL SELECT renders a table" (Invoke-Cmd $port @('SQL', "SELECT * FROM kv")) 'user:1'
    Invoke-Cmd $port @('SQL', "UPDATE kv SET value = 'alicia' WHERE key = 'user:1'") | Out-Null
    Assert-Match "SQL UPDATE changes the value" (Invoke-Cmd $port @('GET', 'user:1')) 'alicia'
    Invoke-Cmd $port @('SQL', "DELETE FROM kv WHERE key = 'user:1'") | Out-Null
    Assert-Match "SQL DELETE removes the key" (Invoke-Cmd $port @('GET', 'user:1')) 'nil'
    Assert-Match "an unqualified DELETE is refused" (Invoke-Cmd $port @('SQL', "DELETE FROM kv")) 'WHERE'

    Stop-Node $sqlNode

    # -----------------------------------------------------------------------
    Write-Host "`nCluster mode" -ForegroundColor Yellow
    $port = $BasePort + 8
    $clusterNode = Start-Node -Name 'cluster' -Port $port -ExtraArgs @('--cluster-enabled', 'yes')

    Assert-Match "CLUSTER INFO reports full slot coverage" (Invoke-Cmd $port @('CLUSTER', 'INFO')) 'cluster_slots_assigned:16384'
    Assert-Match "CLUSTER KEYSLOT matches Redis for 'foo'" (Invoke-Cmd $port @('CLUSTER', 'KEYSLOT', 'foo')) '12182'
    Assert-Match "a single node serves its own slots" (Invoke-Cmd $port @('SET', 'clustered', 'yes')) 'OK'
    $tagged = Invoke-Cmd $port @('CLUSTER', 'KEYSLOT', '{u}:a')
    $tagged2 = Invoke-Cmd $port @('CLUSTER', 'KEYSLOT', '{u}:b')
    Assert-Match "hash tags co-locate keys" $tagged $tagged2

    Stop-Node $clusterNode
}
finally {
    foreach ($process in $script:processes) { Stop-Node $process }

    if (-not $KeepLogs) {
        Remove-Item -Recurse -Force $workDir -ErrorAction SilentlyContinue
    } else {
        Write-Host "`nLogs kept in $workDir" -ForegroundColor DarkGray
    }
}

Write-Host "`n$($script:passed) passed, $($script:failed) failed" -ForegroundColor $(if ($script:failed -eq 0) { 'Green' } else { 'Red' })
if ($script:failures.Count -gt 0) {
    Write-Host "`nFailing checks:" -ForegroundColor Red
    $script:failures | ForEach-Object { Write-Host "  $_" -ForegroundColor Red }
}

exit $(if ($script:failed -eq 0) { 0 } else { 1 })
