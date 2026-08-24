import sys
path, start, end = sys.argv[1], int(sys.argv[2]), int(sys.argv[3])
lines = open(path, encoding='utf-8', errors='replace').read().splitlines()
for i, l in enumerate(lines[start-1:end], start=start):
    print('%4d  %s' % (i, l.encode('cp1252', 'replace').decode('cp1252')))
