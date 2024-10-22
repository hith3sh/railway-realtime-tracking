#ifndef FIXEDSIZECOUNTER_H
#define FIXEDSIZECOUNTER_H


class FixedSizeCounter {
public:
    FixedSizeCounter(int maxSize);
    void add(int value);
    int get_sum() const;
    int get_size() const;
    void reset_counter();

private:
    int* array;
    int size;
    int sum;
    int oldestIndex;
    int maxSize;
};

#endif // FIXEDSIZECOUNTER_H
