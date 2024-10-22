#include "FixedSizeCounter.h"
#include <glib.h>
#include <glog/logging.h>


FixedSizeCounter::FixedSizeCounter(int maxSize) : size(0), sum(0), oldestIndex(0), maxSize(maxSize) {
    array = new int[maxSize];
}

void FixedSizeCounter::add(int value) {
    if (size == maxSize) {
        sum -= array[oldestIndex];
        oldestIndex = (oldestIndex + 1) % maxSize;
        size--;
    }
    
    array[(oldestIndex + size) % maxSize] = value;
    sum += value;
    size++;
}

int FixedSizeCounter::get_sum() const {
    return sum;
}

int FixedSizeCounter::get_size() const {
    return size;
}

void FixedSizeCounter::reset_counter() {
    for (int i = 0; i < size; i++){
        array[i] = 0;
    }
    sum = 0;
    VLOG(2) << "[Deepstream] - [Alarm] - Counter Reset";
}
