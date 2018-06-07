#include <wiringPi.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>

#define MAX_TIME 85
#define DHT11PIN 4

//int dht11_val[5] = {0,0,0,0,0};


#include "pi_2_dht_read.h"
#include "pi_2_mmio.h"

// This is the only processor specific magic value, the maximum amount of time to
// spin in a loop before bailing out and considering the read a timeout.  This should
// be a high value, but if you're running on a much faster platform than a Raspberry
// Pi or Beaglebone Black then it might need to be increased.
#define DHT_MAXCOUNT 32000

// Number of bit pulses to expect from the DHT.  Note that this is 41 because
// the first pulse is a constant 50 microsecond pulse, with 40 pulses to represent
// the data afterwards.
#define DHT_PULSES 41

int pi_2_dht_read(int type, int pin, float* humidity, float* temperature) {
  // Validate humidity and temperature arguments and set them to zero.
  if (humidity == NULL || temperature == NULL) {
    return DHT_ERROR_ARGUMENT;
  }
  *temperature = 0.0f;
  *humidity = 0.0f;

  // Initialize GPIO library.
  if (pi_2_mmio_init() < 0) {
    return DHT_ERROR_GPIO;
  }

  // Store the count that each DHT bit pulse is low and high.
  // Make sure array is initialized to start at zero.
  int pulseCounts[DHT_PULSES*2] = {0};

  // Set pin to output.
  pi_2_mmio_set_output(pin);

  // Bump up process priority and change scheduler to try to try to make process more 'real time'.
  set_max_priority();

  // Set pin high for ~500 milliseconds.
  pi_2_mmio_set_high(pin);
  sleep_milliseconds(500);

  // The next calls are timing critical and care should be taken
  // to ensure no unnecssary work is done below.

  // Set pin low for ~20 milliseconds.
  pi_2_mmio_set_low(pin);
  busy_wait_milliseconds(20);

  // Set pin at input.
  pi_2_mmio_set_input(pin);
  // Need a very short delay before reading pins or else value is sometimes still low.
  for (volatile int i = 0; i < 50; ++i) {
  }

  // Wait for DHT to pull pin low.
  uint32_t count = 0;
  while (pi_2_mmio_input(pin)) {
    if (++count >= DHT_MAXCOUNT) {
      // Timeout waiting for response.
      set_default_priority();
      return DHT_ERROR_TIMEOUT;
    }
  }

  // Record pulse widths for the expected result bits.
  for (int i=0; i < DHT_PULSES*2; i+=2) {
    // Count how long pin is low and store in pulseCounts[i]
    while (!pi_2_mmio_input(pin)) {
      if (++pulseCounts[i] >= DHT_MAXCOUNT) {
        // Timeout waiting for response.
        set_default_priority();
        return DHT_ERROR_TIMEOUT;
      }
    }
    // Count how long pin is high and store in pulseCounts[i+1]
    while (pi_2_mmio_input(pin)) {
      if (++pulseCounts[i+1] >= DHT_MAXCOUNT) {
        // Timeout waiting for response.
        set_default_priority();
        return DHT_ERROR_TIMEOUT;
      }
    }
  }

  // Done with timing critical code, now interpret the results.

  // Drop back to normal priority.
  set_default_priority();

  // Compute the average low pulse width to use as a 50 microsecond reference threshold.
  // Ignore the first two readings because they are a constant 80 microsecond pulse.
  uint32_t threshold = 0;
  for (int i=2; i < DHT_PULSES*2; i+=2) {
    threshold += pulseCounts[i];
  }
  threshold /= DHT_PULSES-1;

  // Interpret each high pulse as a 0 or 1 by comparing it to the 50us reference.
  // If the count is less than 50us it must be a ~28us 0 pulse, and if it's higher
  // then it must be a ~70us 1 pulse.
  uint8_t data[5] = {0};
  for (int i=3; i < DHT_PULSES*2; i+=2) {
    int index = (i-3)/16;
    data[index] <<= 1;
    if (pulseCounts[i] >= threshold) {
      // One bit for long pulse.
      data[index] |= 1;
    }
    // Else zero bit for short pulse.
  }

  // Useful debug info:
  //printf("Data: 0x%x 0x%x 0x%x 0x%x 0x%x\n", data[0], data[1], data[2], data[3], data[4]);

  // Verify checksum of received data.
  if (data[4] == ((data[0] + data[1] + data[2] + data[3]) & 0xFF)) {
    if (type == DHT11) {
      // Get humidity and temp for DHT11 sensor.
      *humidity = (float)data[0];
      *temperature = (float)data[2];
    }
    else if (type == DHT22) {
      // Calculate humidity and temp for DHT22 sensor.
      *humidity = (data[0] * 256 + data[1]) / 10.0f;
      *temperature = ((data[2] & 0x7F) * 256 + data[3]) / 10.0f;
      if (data[2] & 0x80) {
        *temperature *= -1.0f;
      }
    }
    return DHT_SUCCESS;
  }
  else {
    return DHT_ERROR_CHECKSUM;
  }
}


int main(void) {
  float a, b;
  char buf[1024];
  int fd;
  int last_sw_mode = -1;

  printf("Interfacing Temperature and Humidity Sensor (DHT11) With Raspberry Pi\n");
  if(wiringPiSetup()==-1)
    exit(1);
  while(1)
  {
    if ( pi_2_dht_read(DHT11, DHT11PIN, &a, &b) == DHT_SUCCESS ) {
      printf("%lf %lf\n", a, b);
      sprintf(buf, "redis-cli set humidity %d", (int)a);
      system(buf);
      sprintf(buf, "redis-cli set temperature %d", (int)b);
      if ( a < 60 ) {
//        if ( last_sw_mode != 1 ) {
        last_sw_mode = 1;
        fd = open("/dev/switch_dev", O_RDWR);
        write(fd, "blink", 5);
        close(fd);
//        }
      }
      else {
        //if ( last_sw_mode != 0 ) {
        last_sw_mode = 0;
        fd = open("/dev/switch_dev", O_RDWR);
        write(fd, "no blink", 8);
        close(fd);
        //}
      }
      system(buf);
    }
    delay(1000);
  }
  return 0;
}
