#!/usr/bin/env bash
# Robust 30-min observation: checkpoints at ~T+10,T+15,T+20,T+25,T+30 from launch
HZW=/opt/hangzhouwan/current/bin/hzwctl
LOG=/home/linaro/hangzhouwan/docs/production/audit-evidence/observation-loop.log
START=$(date +%s)
check() {
  local label="$1" now elapsed vp_now
  now=$(date --iso-8601=seconds); elapsed=$(( $(date +%s) - START ))
  vp_now=$(systemctl show hangzhouwan-video.service -p MainPID --value 2>/dev/null)
  {
    echo ""
    echo "########## $label | wall=$now | obs_elapsed=${elapsed}s | video_pid=$vp_now ##########"
    systemctl show hangzhouwan-video.service -p ActiveState,SubState,MainPID,NRestarts 2>/dev/null | tr '\n' ' '; echo
    systemctl show hangzhouwan-business.service -p ActiveState,SubState,MainPID,NRestarts 2>/dev/null | tr '\n' ' '; echo
    $HZW health 2>/dev/null
    echo "--- VPU ---"
    bm-smi -noloop 2>/dev/null | sed 's/\x1b\[[0-9;?]*[a-zA-Z]//g' | tr -s ' ' | grep -iE "1684|MB|Process|dual_stream|Active" | head -3
    echo "--- VPU/resource errors (last 10 min this PID) ---"
    journalctl -u hangzhouwan-video.service "_PID=${vp_now}" --since "-10 min" --no-pager -o cat -n 500 2>/dev/null | grep -Eo 'bm_alloc_gmem failed|BMVidDecSeqInitW5 failed|AllocateDecFrameBuffer[^[:cntrl:]]*fail|free gmem[^[:cntrl:]]*invalide|DEVICE_RESOURCE_FATAL|资源致命|Segmentation' | sort | uniq -c | sort -nr | head
    echo "(empty = no VPU/resource errors)"
  } >> "$LOG" 2>&1
}
echo "OBSERVATION START $(date --iso-8601=seconds)" > "$LOG"
# T+10,T+15,T+20,T+25,T+30 from launch (uptime already ~6min)
sleep 240;  check "T+10"
sleep 300;  check "T+15"
sleep 300;  check "T+20"
sleep 300;  check "T+25"
sleep 300;  check "T+30"
echo "OBSERVATION DONE $(date --iso-8601=seconds)" >> "$LOG"
