float approxDiv(float numerator, float denominator)
{
    precise float inverse = 1.0 / denominator;
    precise float product = numerator * inverse;
    return product;
}

float approxSqrt(float value)
{
    precise float root = value * inversesqrt(value);
    return root;
}
