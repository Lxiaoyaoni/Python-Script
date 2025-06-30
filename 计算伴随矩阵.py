import numpy as np
def adjugate_matrix(matrix):
    A = np.array(matrix, dtype=float)
    if A.shape[0] != A.shape[1]:
        raise ValueError("Input matrix must be a square matrix.")

    n = A.shape[0]
    cofactors = np.zeros_like(A)
    for row in range(n):
        for col in range(n):
            minor = A[np.array(list(range(row)) + list(range(row + 1, n))), :][:,
                    np.array(list(range(col)) + list(range(col + 1, n)))]
            cofactors[row, col] = ((-1) ** (row + col)) * np.linalg.det(minor)
    adjugate = cofactors.T
    return adjugate

# 示例：定义一个3x3矩阵
A = [[4, 3, 2],
     [1, 6, 5],
     [9, 8, 7]]

# 计算伴随矩阵
adj_A = adjugate_matrix(A)

print("Original Matrix:")
print(np.array(A))
print("\nAdjugate Matrix:")
print(adj_A)
