#include <cstdlib>
namespace KBM
{
    int rand_r(int &seed)
    {
        seed = seed * 0x343fd + 0x269EC3; // a=214013, b=2531011
        return (seed >> 0x10) & 0x7FFF;
    }

    template <typename T>
    void swap(T &x, T &y)
    {
        T temp = x;
        x = y;
        y = temp;
    }

    template void swap(float &x, float &y);

    template <typename T>
    void bubble_sort(T *arr, int n)
    {
        int i, j;
        for (i = 0; i < n - 1; i++)
        {
            // Last i elements are already in place
            for (j = 0; j < n - i - 1; j++)
            {
                if (arr[j] > arr[j + 1])
                {
                    swap(arr[j], arr[j + 1]);
                }
            }
        }
    }

    template <typename T>
    int qs_partition(T *arr, int start, int end)
    {

        T pivot = arr[start];

        int count = 0;
        for (int i = start + 1; i <= end; i++)
        {
            if (arr[i] <= pivot)
                count++;
        }

        // Giving pivot element its correct position
        int pivotIndex = start + count;
        swap(arr[pivotIndex], arr[start]);

        // Sorting left and right parts of the pivot element
        int i = start, j = end;

        while (i < pivotIndex && j > pivotIndex)
        {

            while (arr[i] <= pivot)
            {
                i++;
            }

            while (arr[j] > pivot)
            {
                j--;
            }

            if (i < pivotIndex && j > pivotIndex)
            {
                swap(arr[i++], arr[j--]);
            }
        }

        return pivotIndex;
    }
    template <>
    int qs_partition(float *arr, int start, int end);

    template <typename T>
    void qs_helper(T *arr, int start, int end)
    {

        // base case
        if (start >= end)
            return;

        // partitioning the array
        int p = qs_partition(arr, start, end);

        // Sorting the left part
        qs_helper(arr, start, p - 1);

        // Sorting the right part
        qs_helper(arr, p + 1, end);
    }

    template <>
    void qs_helper(float *arr, int start, int end);

    template <typename T>
    void quick_sort(T *arr, int n)
    {
        qs_helper(arr, 0, n - 1);
    }

    template <>
    void quick_sort(float *arr, int n);
}

void sorting_kernel(size_t lower, size_t upper)
{
    const size_t offset = 1;
    const size_t scale = 1500;
    for (size_t i = lower; i < upper; i++)
    {
        const size_t n = offset + i * scale;
        float *vec = new float[n];

        int seed = static_cast<int>(i);
        for (size_t i = 0; i < n; i++)
        {
            vec[i] = static_cast<float>(KBM::rand_r(seed)) / static_cast<float>(RAND_MAX);
        }

        KBM::quick_sort(vec, n);
        delete[] vec;
    }
}

int main()
{
    sorting_kernel(0, 200);
}