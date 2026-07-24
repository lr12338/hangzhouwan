#!/usr/bin/env bash
HZW=/opt/hangzhouwan/current/bin/hzwctl
LOG=/home/linaro/hangzhouwan/docs/production/audit-evidence/obs-ptsfix.log
START=$(date +%s)
snap() {
  local label="$1" now elapsed vp
  now=$(date --iso-8601=seconds); elapsed=$(( $(date +%s) - START ))
  vp=$(systemctl show hangzhouwan-video.service -p MainPID --value 2>/dev/null)
  local nr=$(systemctl show hangzhouwan-video.service -p NRestarts --value 2>/dev/null)
  {
    echo "=== $label | $now | +${elapsed}s | pid=$vp nrestarts=$nr ==="
    $HZW health 2>/dev/null | python3 -c "
import json,sys,re
m=re.search(r'\{.*\}',sys.stdin.read(),re.S)
d=json.loads(m.group(0)) if m else {}
print('status=%s degr=%s uptime=%s'%(d.get('status'),d.get('degradation'),d.get('uptime_seconds')))
for s in ['A','B']:
    st=d.get('streams',{}).get(s,{})
    print('  %s %s rtsp=%s rtmp=%s out=%.2f inf=%.2f rtsp_rc=%s rtmp_rc=%s q=%s'%(s,st.get('level'),st.get('rtsp_connected'),st.get('rtmp_connected'),st.get('output_fps',0),st.get('inference_fps',0),st.get('rtsp_reconnects'),st.get('rtmp_reconnects'),st.get('queue_length')))
" 2>&1
    echo -n "VPUerr(last10m)=" 
    journalctl -u hangzhouwan-video.service "_PID=${vp}" --since "-10 min" --no-pager -o cat -n 500 2>/dev/null | grep -cE 'bm_alloc_gmem failed|BMVidDecSeqInitW5 failed|DEVICE_RESOURCE_FATAL'
    echo -n "pts<dts(last5m)=" 
    journalctl -u hangzhouwan-video.service "_PID=${vp}" --since "-5 min" --no-pager -o cat -n 800 2>/dev/null | grep -c 'pts.*<.*dts'
    echo -n "RTMP重连日志(last5m)=" 
    journalctl -u hangzhouwan-video.service "_PID=${vp}" --since "-5 min" --no-pager -o cat -n 800 2>/dev/null | grep -c 'RTMP重连'
  } >> "$LOG" 2>&1
}
echo "PTS-FIX OBSERVATION START $(date --iso-8601=seconds)" > "$LOG"
sleep 110; snap "T+2"
sleep 180; snap "T+5"
sleep 300; snap "T+10"
sleep 300; snap "T+15"
echo "PTS-FIX OBSERVATION DONE $(date --iso-8601=seconds)" >> "$LOG"
