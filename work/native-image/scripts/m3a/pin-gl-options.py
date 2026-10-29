#!/usr/bin/env python3
# pin-gl-options.py <options.txt>  (M3 stage C, 26.x): force the OpenGL backend.
# 26.2 Options key `preferredGraphicsBackend`, value = PreferredGraphicsApi serialized name: "default" | "opengl" | "vulkan"
# (JSON string in options.txt). Minecraft.<init>: DEFAULT also runs VulkanBackend.checkBackendAvailable(); every value
# tries [GL, Vulkan] (VULKAN: [Vulkan, GL]). OPENGL = GL only, no Vulkan probe unless GL fails.
# Caveat: DFU OptionsForceDefaultGraphicsApiFix rewrites the key to "default" when options.txt's `version:` is older than
# that fix's schema, so pin AFTER the instance's first 26.2 run has rewritten the file (version:4903 for 26.2).
import os, sys
p = sys.argv[1]
lines = open(p).read().splitlines()
ver = next((int(l.split(':', 1)[1]) for l in lines if l.startswith('version:')), 0)
out = [l for l in lines if not l.startswith('preferredGraphicsBackend:')]
out.append('preferredGraphicsBackend:"opengl"')
# Launch 10 (perf parity with 1.21.5, the instance Austin called smooth): 1.21.5 ran Sodium leaves_quality FAST (opaque
# leaves, culled between blocks); 26.x moved leaves into vanilla `cutoutLeaves` (default true = cut-out "fancy" leaves:
# every inner leaf face drawn + alpha test, the most expensive geometry in a forest). KL_VIVECRAFT_FANCY_LEAVES=1 keeps
# the file's value.
if os.environ.get('KL_VIVECRAFT_FANCY_LEAVES', '0') != '1':
    out = [('cutoutLeaves:false' if l.startswith('cutoutLeaves:') else l) for l in out]
    if not any(l.startswith('cutoutLeaves:') for l in out): out.append('cutoutLeaves:false')
open(p + '.tmp', 'w').write('\n'.join(out) + '\n'); os.replace(p + '.tmp', p)
print(f'[stage] options: preferredGraphicsBackend:"opengl", {next(l for l in out if l.startswith("cutoutLeaves:"))} (options version {ver}'
      + (', OLDER than 26.2 = 4903: the DFU fix will reset it to "default" on first boot)' if ver < 4903 else ')'))
