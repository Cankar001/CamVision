#pragma once

#include "Core.h"

#include <assert.h>

namespace Core
{
    template <class T>
    class RingBuffer
    {
    public:

        RingBuffer(uint32 capacity)
        {
            m_ReadPos = 0;
            m_WritePos = 0;
            m_Size = 0;
            m_Capacity = capacity;
            m_Data = ((T *)operator new(m_Capacity * sizeof(T)));
        }

        ~RingBuffer()
        {
            // first destroy any content
            while (m_Size != 0)
                Pop();

            // then release the buffer.
            if (m_Data)
            {
                operator delete(m_Data);
                m_Data = nullptr;
            }
        }

        /// <summary>
        /// Pushes a new object into the buffer.
        /// </summary>
        /// <param name="value">The value to push into the ringbuffer.</param>
        void Push(T const &value)
        {
            // ensure there's room in buffer:
            if (m_Size == m_Capacity)
                Pop();

            // construct copy of object in-place into buffer
            new(&m_Data[m_WritePos]) T(value);

            ++m_WritePos;

            // keep pointer in bounds.
            m_WritePos %= m_Capacity;
            ++m_Size;
        }

        /// <summary>
        /// return oldest object in queue.
        /// </summary>
        /// <returns>Returns the oldest object from the queue.</returns>
        T Front()
        {
            return m_Data[m_ReadPos];
        }

        /// <summary>
        /// remove oldest object from queue.
        /// </summary>
        void Pop()
        {
            // destroy the object:
            m_Data[m_ReadPos++].~T();

            // keep pointer in bounds.
            m_ReadPos %= m_Capacity;
            --m_Size;
        }

        uint32 Size() const
        {
            return m_Size;
        }

        RingBuffer<T> Copy()
        {
            RingBuffer<T> copy(m_Capacity);

            while (m_Size != 0)
            {
                copy.Push(Front());
                Pop();
            }

            // then release the buffer.
            if (m_Data)
            {
                operator delete(m_Data);
                m_Data = nullptr;
            }
            
            return copy;
        }

        void Clear()
        {
            while (m_Size != 0)
                Pop();
        }

        /// <summary>
        /// The element at a position counted from the oldest one (0 is the oldest, Size() - 1 the newest). Unlike operator[], which is the position in the memory.
        /// </summary>
        T &At(uint32 index)
        {
            assert(index < m_Size);
            return m_Data[(m_ReadPos + index) % m_Capacity];
        }

        T &operator[](uint32 index)
        {
            assert(index < m_Size);
            return m_Data[index];
        }

        const T &operator[](uint32 index) const
        {
            assert(index < m_Size);
            return m_Data[index];
        }

    private:

        T *m_Data = nullptr;
        uint32 m_ReadPos = 0;
        uint32 m_WritePos = 0;
        uint32 m_Size = 0;
        uint32 m_Capacity = 0;
    };
}

