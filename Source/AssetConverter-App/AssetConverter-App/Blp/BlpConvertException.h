#pragma once

#include <exception>
#include <string>
#include <utility>

namespace BLP 
{
    class BlpConvertException : public std::exception 
    {
        std::string mMessage;

    public:
        explicit BlpConvertException(const std::string& message) : mMessage(message) 
        {

        }

        virtual ~BlpConvertException() throw() { }

        const char *what() const throw() 
        {
            return mMessage.c_str();
        }
    };
}
