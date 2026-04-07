#ifndef A_TOUCH_EVENT_H // !A_TOUCH_EVENT_H
#define A_TOUCH_EVENT_H

#include <fcntl.h>
#include <unistd.h>
#include <linux/input.h>
#include <android/keycodes.h>

#include <array>
#include <algorithm>
#include <bitset>
#include <filesystem>
#include <vector>

namespace android
{
    namespace detail
    {
        template <std::size_t BITS>
        class BitArray
        {
            /* Array element type and vector of element type. */
            using Element = std::uint32_t;
            /* Number of bits in each BitArray element. */
            static constexpr size_t WIDTH = sizeof(Element) * CHAR_BIT;
            /* Number of elements to represent a bit array of the specified size of bits. */
            static constexpr size_t COUNT = (BITS + WIDTH - 1) / WIDTH;

        public:
            /* BUFFER type declaration for BitArray */
            using Buffer = std::array<Element, COUNT>;
            /* To tell if a bit is set in array, it selects an element from the array, and test
             * if the relevant bit set.
             * Note the parameter "bit" is an index to the bit, 0 <= bit < BITS.
             */
            inline bool test(size_t bit) const
            {
                return (bit < BITS) ? mData[bit / WIDTH].test(bit % WIDTH) : false;
            }
            /* Returns total number of bytes needed for the array */
            inline size_t bytes() { return (BITS + CHAR_BIT - 1) / CHAR_BIT; }
            /* Returns true if array contains any non-zero bit from the range defined by start and end
             * bit index [startIndex, endIndex).
             */
            bool any(size_t startIndex, size_t endIndex);
            /* Load bit array values from buffer */
            void loadFromBuffer(const Buffer &buffer)
            {
                for (size_t i = 0; i < COUNT; i++)
                {
                    mData[i] = std::bitset<WIDTH>(buffer[i]);
                }
            }

        private:
            std::array<std::bitset<WIDTH>, COUNT> mData;
        };
    }

    class ATouchEvent
    {
    public:
        enum class EventType : uint32_t
        {
            Move,
            TouchDown,
            TouchUp,
            KeyDown,
            KeyUp,
            Wheel,
        };

        struct TouchEvent
        {
            EventType type;
            int x;
            int y;
            int scanCode;
            int keyCode;

            void TransformToScreen(int width, int height, int theta = 0)
            {
                if (transformScalerX <= 0 || transformScalerY <= 0 || width <= 0 || height <= 0)
                    return;

                const int rawX = x;
                const int rawY = y;
                int mappedX = rawX;
                int mappedY = rawY;
                int sourceWidth = transformScalerX;
                int sourceHeight = transformScalerY;

                theta %= 360;
                if (theta < 0)
                    theta += 360;

                if (90 == theta)
                {
                    mappedX = rawY;
                    mappedY = transformScalerX - rawX;
                    sourceWidth = transformScalerY;
                    sourceHeight = transformScalerX;
                }
                else if (180 == theta)
                {
                    mappedX = transformScalerX - rawX;
                    mappedY = transformScalerY - rawY;
                }
                else if (270 == theta)
                {
                    mappedX = transformScalerY - rawY;
                    mappedY = rawX;
                    sourceWidth = transformScalerY;
                    sourceHeight = transformScalerX;
                }

                x = mappedX * width / sourceWidth;
                y = mappedY * height / sourceHeight;
                x = std::clamp(x, 0, width - 1);
                y = std::clamp(y, 0, height - 1);
            }
        };

    public:
        ATouchEvent();
        ~ATouchEvent();

        bool GetRawEvent(input_event *event);

        bool GetTouchEvent(TouchEvent *touchEvent);

    public:
        static int transformScalerX, transformScalerY;

    private:
        int m_deviceFd = -1;

        typename detail::BitArray<KEY_MAX>::Buffer m_keyBitBuffer;
        detail::BitArray<KEY_MAX> m_keyBitmask;
    };
}

#endif // !A_TOUCH_EVENT_H
