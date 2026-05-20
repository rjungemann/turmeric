import sys

def matmul(n):
    A = [[1.0] * n for _ in range(n)]
    B = [[1.0] * n for _ in range(n)]
    C = [[0.0] * n for _ in range(n)]
    for i in range(n):
        for k in range(n):
            for j in range(n):
                C[i][j] += A[i][k] * B[k][j]
    return sum(C[i][j] for i in range(n) for j in range(n))

n = int(sys.argv[1]) if len(sys.argv) > 1 else 10
print(int(matmul(n)))
