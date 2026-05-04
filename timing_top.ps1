$csvPath='G:\hotwire\.godot\editor\import_asset_timings.csv'
$rows = Import-Csv $csvPath | ForEach-Object {
 [PSCustomObject]@{ts=[int64]$_.unix_time_ms; dur=[int64]$_.duration_ms; importer=[string]$_.importer; phase=[string]$_.phase; threaded=[int]$_.threaded; asset=[string]$_.asset_path; threads=[int]$_.threads_used; pool=[int]$_.thread_pool_size; batch=[int]$_.batch_size}
}
$max=($rows|Measure-Object ts -Maximum).Maximum
$recent=$rows|Where-Object{ $_.ts -ge ($max-30*60*1000)}
Write-Output 'TOP_SCENE_ASSET_RECENT'
$recent|Where-Object{ $_.importer -eq 'scene' -and $_.phase -eq 'asset' }|Sort-Object dur -Descending|Select-Object -First 20|ForEach-Object{"{0},{1},{2},{3},{4}" -f $_.dur,$_.threads,$_.pool,$_.batch,$_.asset}
Write-Output 'TOP_CUSTOM_ASSET_RECENT'
$recent|Where-Object{ $_.importer -eq '<custom_or_auto>' -and $_.phase -eq 'asset' }|Sort-Object dur -Descending|Select-Object -First 20|ForEach-Object{"{0},{1},{2},{3},{4}" -f $_.dur,$_.threaded,$_.threads,$_.pool,$_.asset}
Write-Output 'COMMIT_STATS_RECENT'
$commit=$recent|Where-Object{ $_.importer -eq 'scene' -and $_.phase -eq 'commit_done' }
Write-Output ("COMMIT_COUNT={0}" -f $commit.Count)
Write-Output ("COMMIT_SUM_MS={0}" -f (($commit|Measure-Object dur -Sum).Sum))
Write-Output ("COMMIT_MAX_MS={0}" -f (($commit|Measure-Object dur -Maximum).Maximum))
if($commit.Count -gt 0){
  $ordered=$commit|Select-Object -ExpandProperty dur|Sort-Object
  $p95=$ordered[[Math]::Floor(0.95*($commit.Count-1))]
  Write-Output ("COMMIT_P95_MS={0}" -f $p95)
}
Write-Output 'SCENE_COMPLETION_GAPS_MS_TOP'
$done=$recent|Where-Object{ $_.importer -eq 'scene' -and $_.phase -eq 'worker_done' }|Sort-Object ts
$gaps = for($i=1;$i -lt $done.Count;$i++){
  $gap=[int64]($done[$i].ts-$done[$i-1].ts)
  [PSCustomObject]@{gap=$gap;prev=$done[$i-1].asset;next=$done[$i].asset;next_dur=$done[$i].dur}
}
$gaps|Sort-Object gap -Descending|Select-Object -First 12|ForEach-Object{"{0},{1},{2},{3}" -f $_.gap,$_.next_dur,$_.prev,$_.next}
Write-Output 'ASSET_EXT_BREAKDOWN_RECENT'
$recent|Where-Object{$_.phase -eq 'asset'}|ForEach-Object{ [PSCustomObject]@{ext=[IO.Path]::GetExtension($_.asset).ToLowerInvariant(); dur=$_.dur; importer=$_.importer} }|
Group-Object ext,importer|ForEach-Object{ $sum=(($_.Group|Measure-Object dur -Sum).Sum); [PSCustomObject]@{key=$_.Name; count=$_.Count; sum=[int64]$sum; avg=[int64]([math]::Round($sum/[double]$_.Count))}}|Sort-Object sum -Descending|Select-Object -First 20|ForEach-Object{"{0},{1},{2},{3}" -f $_.key,$_.count,$_.sum,$_.avg}
