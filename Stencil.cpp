#include <stdlib.h>
#include <omp.h>
#include <iostream>
#include <cstring>

void createMAtrix(float* A, int n)
{
    for (int i = 0; i < n; i++){
        for (int j = 0; j < n; j++){
            A[i * n + j] = (i * n) + j;
        }
    }
}

void stencil2D(float* A, float* result, int n)
{
#pragma omp parallel
    {
#pragma omp single
        {
            std::cout << "Max threads available: " << omp_get_max_threads() << std::endl;
            std::cout << "Number of threads (actual): "<< omp_get_num_threads() << std::endl;
        }

#pragma omp for collapse(2) schedule(static)
        for (int j = 1; j < n - 1; ++j) {
            for (int k = 1; k < n - 1; ++k) {
                result[j * n + k] = 0.2f * (
                    A[j * n + k] +
                    A[(j - 1) * n + k] +
                    A[(j + 1) * n + k] +
                    A[j * n + (k - 1)] +
                    A[j * n + (k + 1)]
                );
            }
        }
    }
}

int main(int argc, char* argv[])
{
    if (argc < 2) {
        std::cerr << "Usage: " << argv[0] << " N\n";
        return 1;
    }

    int n = atoi(argv[1]);

    float* A = (float*)malloc(n * n * sizeof(float));
    float* result = (float*)malloc(n * n * sizeof(float));

    createMAtrix(A, n);
    memset(result, 0, n * n * sizeof(float));

    double timestart = omp_get_wtime();
    stencil2D(A, result, n);
    double timeend = omp_get_wtime();

    std::cout << "Time elapsed: " << timeend - timestart << std::endl;

    free(A);
    free(result);

    return 0;
}