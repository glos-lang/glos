import errno, os, re
names = [n for n in dir(errno) if re.fullmatch(r'E[A-Z0-9]+', n)]
for name in sorted(names, key=lambda n: getattr(errno, n)):
    v = getattr(errno, name)
    print(f"{name} \"{os.strerror(v)}\" = {v}")
