import sys
import threading
import queue

n_threads = int(sys.argv[1]) if len(sys.argv) > 1 else 4
messages  = int(sys.argv[2]) if len(sys.argv) > 2 else 1000

chans = [queue.Queue(maxsize=1) for _ in range(n_threads)]
done  = threading.Event()

# Adjust so the ring completes at thread 0 (messages must be a multiple of n_threads)
adj = (n_threads - messages % n_threads) % n_threads
total = messages + adj

def worker(i):
    me  = chans[i]
    nxt = chans[(i + 1) % n_threads]
    while True:
        tok = me.get()
        if tok <= 0:
            if i == 0:
                done.set()
            else:
                nxt.put(tok - 1)
            return
        nxt.put(tok - 1)

threads = [threading.Thread(target=worker, args=(i,), daemon=True) for i in range(n_threads)]
for t in threads: t.start()
chans[0].put(total)
done.wait()
print("done")
