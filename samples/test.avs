LoadPlugin("..\build\src\avs\Release\mxfsource.dll")
# Phase 0 scaffold args (width/height/num_frames/fps) are placeholders -- once
# the Phase 1+ MXF parser lands, MxfSource will read these from the file itself.
MxfSource("path\to\file.mxf", width=1920, height=1080, num_frames=300, fps_num=25, fps_den=1)
