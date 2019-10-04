import sys, math

if len(sys.argv) < 2:
	print "Usage: %s diameter [rad0 rad1] [\"oddeven\"|\"quarter\"]"
	exit(0)

d = int(sys.argv[1])
if (d % 2) == 0 or d < 1:
	print "Diameter must be an odd positive integer and must be >= 3."
	exit(0)

density = "all"
for arg in sys.argv:
	if arg == "oddeven" or arg == "quarter" or arg=="onefifth":
		density = arg

rad0 = 0
rad1 = 9999
if len(sys.argv) > 2:
	rad0 = float(sys.argv[2])
if len(sys.argv) > 3:
	rad1 = float(sys.argv[3])

outsz = "%d,%d" % (d, d)
print "=" * d
for y in range(0, d):
	for x in range(0, d):
		val = 1
		dx = abs(x - (d-1)/2); dy = abs(y - (d-1)/2)
		if dx==0 and dy==0: sys.stdout.write(" "); continue
		r = math.sqrt(dx*dx + dy*dy);
		if r <= rad1 and r >= rad0:
			if density == "oddeven":
				if ((x+y) % 2)==1: val=1
				else: val = 0
			elif density == "quarter":
				if ((x + y/2) % 2)==0 and (y % 2)==0: val=1
				else: val = 0
			elif density == "onefifth":
				if ((1000 + x - 3*y) % 5) == 0: val = 1
				else: val = 0
			else:
				val = 1
		else:  val = 0
		outsz += ",%d" % val
		if (val == 1): sys.stdout.write("*")
		else: sys.stdout.write(".")
	sys.stdout.write("\n")
print "=" * d
print outsz
