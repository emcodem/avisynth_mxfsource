import vapoursynth as vs

core = vs.core
core.std.LoadPlugin("../build/src/vs/Release/mxfsource_vs.dll")

clip = core.mxfsource.MxfSource(file="path/to/file.mxf", buffer_frames=300)
clip.set_output()
