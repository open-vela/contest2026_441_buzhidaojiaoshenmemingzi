/* SPDX-License-Identifier: Apache-2.0 */

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include <vision_badge/http_date.h>

static int vision_ascii_case_equal(const char *left, const char *right,
                                   size_t count)
{
  size_t i;

  for (i = 0; i < count; i++)
    {
      unsigned char a = (unsigned char)left[i];
      unsigned char b = (unsigned char)right[i];

      if (a >= 'A' && a <= 'Z')
        {
          a = (unsigned char)(a - 'A' + 'a');
        }

      if (b >= 'A' && b <= 'Z')
        {
          b = (unsigned char)(b - 'A' + 'a');
        }

      if (a != b)
        {
          return 0;
        }
    }

  return 1;
}

static int vision_month_number(const char *name)
{
  static const char months[] = "JanFebMarAprMayJunJulAugSepOctNovDec";
  int month;

  for (month = 0; month < 12; month++)
    {
      if (vision_ascii_case_equal(name, months + month * 3, 3))
        {
          return month + 1;
        }
    }

  return 0;
}

static int vision_days_in_month(int year, int month)
{
  static const uint8_t days[] =
  {
    31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31
  };
  int value = days[month - 1];

  if (month == 2 && (year % 4 == 0) &&
      ((year % 100 != 0) || (year % 400 == 0)))
    {
      value++;
    }

  return value;
}

static int64_t vision_days_from_civil(int year, unsigned int month,
                                      unsigned int day)
{
  int era;
  unsigned int year_of_era;
  unsigned int adjusted_month;
  unsigned int day_of_year;
  unsigned int day_of_era;

  year -= month <= 2;
  era = (year >= 0 ? year : year - 399) / 400;
  year_of_era = (unsigned int)(year - era * 400);
  adjusted_month = month > 2 ? month - 3 : month + 9;
  day_of_year = (153 * adjusted_month + 2) / 5 +
                day - 1;
  day_of_era = year_of_era * 365 + year_of_era / 4 - year_of_era / 100 +
               day_of_year;
  return (int64_t)era * 146097 + (int64_t)day_of_era - 719468;
}

static int vision_http_date_value(const char *value, time_t *epoch)
{
  char weekday[4] = {0};
  char month_name[4] = {0};
  int day;
  int month;
  int year;
  int hour;
  int minute;
  int second;
  int consumed = 0;
  int64_t seconds;
  time_t converted;

  if (sscanf(value, "%3s, %d %3s %d %d:%d:%d GMT%n",
             weekday, &day, month_name, &year, &hour, &minute, &second,
             &consumed) != 7)
    {
      return -EBADMSG;
    }

  while (value[consumed] == ' ' || value[consumed] == '\t')
    {
      consumed++;
    }

  if (value[consumed] != '\0' || strlen(weekday) != 3)
    {
      return -EBADMSG;
    }

  month = vision_month_number(month_name);
  if (year < 2024 || year > 2099 || month == 0 || day < 1 ||
      day > vision_days_in_month(year, month) || hour < 0 || hour > 23 ||
      minute < 0 || minute > 59 || second < 0 || second > 60)
    {
      return -ERANGE;
    }

  seconds = vision_days_from_civil(year, (unsigned int)month,
                                   (unsigned int)day) * 86400 +
            hour * 3600 + minute * 60 + second;
  converted = (time_t)seconds;
  if ((int64_t)converted != seconds)
    {
      return -ERANGE;
    }

  *epoch = converted;
  return 0;
}

int vision_http_date_parse(const char *headers, time_t *epoch)
{
  const char *line;

  if (headers == NULL || epoch == NULL)
    {
      return -EINVAL;
    }

  line = headers;
  while (*line != '\0')
    {
      const char *end = strstr(line, "\r\n");
      size_t length = end == NULL ? strlen(line) : (size_t)(end - line);

      if (length == 0)
        {
          break;
        }

      if (length > 5 && vision_ascii_case_equal(line, "Date:", 5))
        {
          char value[64];
          const char *start = line + 5;
          size_t value_size;

          while (start < line + length && (*start == ' ' || *start == '\t'))
            {
              start++;
            }

          value_size = (size_t)(line + length - start);
          if (value_size == 0 || value_size >= sizeof(value))
            {
              return -EBADMSG;
            }

          memcpy(value, start, value_size);
          value[value_size] = '\0';
          return vision_http_date_value(value, epoch);
        }

      if (end == NULL)
        {
          break;
        }

      line = end + 2;
    }

  return -ENOENT;
}
