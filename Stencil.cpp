#include <stdlib.h>
#include <omp.h>
#include <vector>
#include <iostream>

//g++ Stencil.cpp -o stencil -fopenmp
// OMP_NUM_THREADS=6 ./stencil 18000

/*void createMAtrix(std::vector<std::vector<float>>& A, int n)
{
    for (int i = 0; i < n; i++){
        for (int j = 0; j < n; j++){
            A[i][j] = rand() % 100;
        }
    }

}*/

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

#pragma omp for collapse(2)
        for (int j = 1; j < n - 1; ++j) {
            for (int k = 1; k < n - 1; ++k) {
                result[j * n + j] = 0.25 * (
                  A[(j - 1) * n + j] +
                  A[(j + 1) * n + j] +
                  A[j * n + (j - 1)] +
                  A[j * n + (j + 1)]
                  );
            }
        }
    }
}

/*
void stencil2D(std::vector<std::vector<float>>& A, std::vector<std::vector<float>>& result, int n)
{
#pragma omp parallel
    {
#pragma omp single
        {
            std::cout << "Max threads available: " << omp_get_max_threads() << std::endl;
            std::cout << "Number of threads (actual): "<< omp_get_num_threads() << std::endl;
        }

#pragma omp for collapse(2)
        for (int j = 1; j < n - 1; ++j) {
            for (int k = 1; k < n - 1; ++k) {
                result[j][k] =
                    0.25f * A[j][k] +
                    0.25f * A[j][k - 1] +
                    0.25f * A[j][k + 1] +
                    0.25f * A[j - 1][k] +
                    0.25f * A[j + 1][k];
            }
        }
    }
}
*/


int main(int argc, char* argv[])
{
    if (argc < 2) {
        std::cerr << "Usage: " << argv[0] << " N\n";
        return 1;
    }

    //omp_set_dynamic(0);

    //int nthreads = omp_get_num_threads();


    //Size of matrix
    int n = atoi(argv[1]);

    //Stencil

    float* A = (float*)malloc(n * n * sizeof(float));
    float* result = (float*)malloc(n * n * sizeof(float));

    //std::vector<std::vector<float>> A(n, std::vector<float>(n));
    //std::vector<std::vector<float>> result(n, std::vector<float>(n));

    createMAtrix(A,n);

    double timestart = omp_get_wtime();
    stencil2D(A, result, n);
    double timeend = omp_get_wtime();

    std::cout<<"Time elapsed: "<<timeend-timestart<<std::endl;




}