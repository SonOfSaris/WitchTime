# Builds README-WitchTime.txt (readme_head.txt + the third-party notices) and ../dist/release/WitchTime.zip (entries
# under crmods/WitchTime/) from out/witchtime.dll and the package files in this folder. usage: python package.py [zip path]
import hashlib, os, sys, zipfile
src = os.path.dirname(os.path.abspath(__file__))
head = open(os.path.join(src, 'readme_head.txt'), encoding='utf-8').read().rstrip('\n')
notices = open(os.path.join(src, 'notices_readme.txt'), encoding='utf-8').read().rstrip('\n')
open(os.path.join(src, 'README-WitchTime.txt'), 'w', encoding='utf-8', newline='\r\n').write(head + '\n\n' + notices + '\n')
dst = sys.argv[1] if len(sys.argv) > 1 else os.path.join(src, '..', 'dist', 'release', 'WitchTime.zip')
os.makedirs(os.path.dirname(os.path.abspath(dst)), exist_ok=True)
files = ['out/witchtime.dll', 'witchtime_config.ini', 'witchtime.menu.json', 'README-WitchTime.txt']
with zipfile.ZipFile(dst, 'w', zipfile.ZIP_DEFLATED, compresslevel=9) as z:
    for f in files: z.write(os.path.join(src, f), 'crmods/WitchTime/' + os.path.basename(f))
data = open(dst, 'rb').read()
print(os.path.normpath(dst), len(data), 'bytes sha256', hashlib.sha256(data).hexdigest()[:16])
for i in zipfile.ZipFile(dst).infolist(): print('  ', i.filename, i.file_size)
