$csvPath = 'G:\hotwire\.godot\editor\import_asset_timings.csv'
$rows = Import-Csv -Path $csvPath | ForEach-Object {
    [PSCustomObject]@{
        unix_time_ms = [int64]$_.unix_time_ms
        duration_ms = [int64]$_.duration_ms
        threaded = [int]$_.threaded
        result_code = [int]$_.result_code
        importer = [string]$_.importer
        phase = [string]$_.phase
        threads_used = [int]$_.threads_used
        thread_pool_size = [int]$_.thread_pool_size
        batch_size = [int]$_.batch_size
        thread_id = [int64]$_.thread_id
        asset_path = [string]$_.asset_path
    }
}

if(-not $rows){ Write-Output 'NO_ROWS'; exit 0 }
$maxTs = ($rows | Measure-Object unix_time_ms -Maximum).Maximum
$windowMs = 30 * 60 * 1000
$recent = $rows | Where-Object { $_.unix_time_ms -ge ($maxTs - $windowMs) }

Write-Output "RECENT_MIN_TS=$((($recent | Measure-Object unix_time_ms -Minimum).Minimum))"
Write-Output "RECENT_MAX_TS=$((($recent | Measure-Object unix_time_ms -Maximum).Maximum))"
$recentMin = (($recent | Measure-Object unix_time_ms -Minimum).Minimum)
$recentMax = (($recent | Measure-Object unix_time_ms -Maximum).Maximum)
Write-Output "RECENT_SPAN_MS=$($recentMax - $recentMin)"
Write-Output "RECENT_ROWS=$($recent.Count)"

$startCounts = @{}
$doneCounts = @{}
foreach($r in $recent){
    if($r.phase -eq 'asset_start'){
        if(-not $startCounts.ContainsKey($r.asset_path)){ $startCounts[$r.asset_path]=0 }
        $startCounts[$r.asset_path]++
    } elseif($r.phase -eq 'asset'){
        if(-not $doneCounts.ContainsKey($r.asset_path)){ $doneCounts[$r.asset_path]=0 }
        $doneCounts[$r.asset_path]++
    }
}
$unfinished = @()
foreach($k in $startCounts.Keys){
    $s = $startCounts[$k]
    $d = 0
    if($doneCounts.ContainsKey($k)){ $d = $doneCounts[$k] }
    if($s -gt $d){
        $unfinished += [PSCustomObject]@{asset=$k; starts=$s; dones=$d; missing=($s-$d)}
    }
}
Write-Output "UNFINISHED_COUNT=$($unfinished.Count)"
$unfinished | Sort-Object -Property @{Expression='missing';Descending=$true}, @{Expression='asset';Descending=$false} | Select-Object -First 25 | ForEach-Object { Write-Output ("UNFINISHED,{0},{1},{2},{3}" -f $_.missing,$_.starts,$_.dones,$_.asset) }

function Get-Pairs([object[]]$setRows, [string]$startPhase, [string]$donePhase){
    $starts = @{}
    $pairs = New-Object System.Collections.Generic.List[object]
    $ordered = $setRows | Sort-Object unix_time_ms
    foreach($r in $ordered){
        if($r.phase -eq $startPhase){
            if(-not $starts.ContainsKey($r.asset_path)){ $starts[$r.asset_path] = New-Object System.Collections.Generic.Queue[object] }
            $starts[$r.asset_path].Enqueue($r)
        } elseif($r.phase -eq $donePhase){
            if($starts.ContainsKey($r.asset_path) -and $starts[$r.asset_path].Count -gt 0){
                $st = $starts[$r.asset_path].Dequeue()
                $pairs.Add([PSCustomObject]@{
                    asset = $r.asset_path
                    start_ms = [int64]$st.unix_time_ms
                    end_ms = [int64]$r.unix_time_ms
                    dur_ms = [int64]$r.duration_ms
                    thread_id = [int64]$r.thread_id
                })
            }
        }
    }
    return $pairs
}

function Print-OverlapStats([string]$name, [object[]]$pairs){
    if(-not $pairs -or $pairs.Count -eq 0){
        Write-Output ("{0}_PAIRS=0" -f $name)
        return
    }
    $sumDur = [int64](($pairs | Measure-Object dur_ms -Sum).Sum)
    $startMin = [int64](($pairs | Measure-Object start_ms -Minimum).Minimum)
    $endMax = [int64](($pairs | Measure-Object end_ms -Maximum).Maximum)
    $span = [int64]($endMax - $startMin)
    $avgConc = if($span -gt 0){ [double]$sumDur / [double]$span } else { 0.0 }

    $events = New-Object System.Collections.Generic.List[object]
    foreach($p in $pairs){
        $events.Add([PSCustomObject]@{t=$p.start_ms; d=1})
        $events.Add([PSCustomObject]@{t=$p.end_ms; d=-1})
    }
    $active=0; $maxActive=0
    foreach($e in ($events | Sort-Object -Property @{Expression='t';Ascending=$true}, @{Expression='d';Descending=$true})){
        $active += $e.d
        if($active -gt $maxActive){ $maxActive = $active }
    }

    Write-Output ("{0}_PAIRS={1}" -f $name,$pairs.Count)
    Write-Output ("{0}_SUM_DUR_MS={1}" -f $name,$sumDur)
    Write-Output ("{0}_SPAN_MS={1}" -f $name,$span)
    Write-Output ("{0}_AVG_CONC={1:N2}" -f $name,$avgConc)
    Write-Output ("{0}_MAX_CONC={1}" -f $name,$maxActive)
}

$sceneSet = $recent | Where-Object { $_.importer -eq 'scene' }
$scenePairs = Get-Pairs -setRows $sceneSet -startPhase 'worker_start' -donePhase 'worker_done'
Print-OverlapStats -name 'SCENE' -pairs $scenePairs

$customSet = $recent | Where-Object { $_.importer -eq '<custom_or_auto>' }
$customPairs = Get-Pairs -setRows $customSet -startPhase 'asset_start' -donePhase 'asset'
Print-OverlapStats -name 'CUSTOM' -pairs $customPairs

Write-Output "RECENT_THREAD_USAGE"
$recent | Where-Object { $_.phase -in @('asset','worker_done','commit_done') } |
    Group-Object threaded,threads_used,thread_pool_size |
    Sort-Object Count -Descending |
    Select-Object -First 20 |
    ForEach-Object { Write-Output ("THREAD_USAGE,{0},{1}" -f $_.Name,$_.Count) }

Write-Output "RECENT_IMPORTER_SUMMARY"
$recent | Where-Object { $_.phase -eq 'asset' } |
    Group-Object importer |
    ForEach-Object {
        $sum = ($_.Group | Measure-Object duration_ms -Sum).Sum
        $cnt = $_.Count
        $ordered = ($_.Group | Select-Object -ExpandProperty duration_ms | Sort-Object)
        $p95idx = [Math]::Floor(0.95*($cnt-1))
        $p95 = $ordered[$p95idx]
        [PSCustomObject]@{ importer=$_.Name; count=$cnt; total_ms=[int64]$sum; avg_ms=[int64]([math]::Round($sum/[double]$cnt)); p95_ms=[int64]$p95 }
    } |
    Sort-Object total_ms -Descending |
    ForEach-Object { Write-Output ("IMPORTER,{0},{1},{2},{3},{4}" -f $_.importer,$_.count,$_.total_ms,$_.avg_ms,$_.p95_ms) }
