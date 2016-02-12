//map :: (array: [] $T, f: (T) -> $R) -> [] R {
//}

//array_unordered_remove :: inline (array: *[..] $T, item: T) -> s64 {
//    return 0;
//}

static const int REPEATING = 0x1;

struct Vector2
{
    float x; // : float = 1;
    float y; // : float = 2;
};

Vector2 vec = { 1, 2 };
float gPi = 3.14159f;

int printf(const char *, ...);

long long Factorial(long long n)
{
    if (n == 0)
        return 1; 

    return n * Factorial(n-1); 
}

int main ()
{
	signed char i = 0; // BB (adrianb) What's the right type for this? s64?
    long long c = 5;
    static const signed char a = 2;
    short b = 900;
    Vector2 aVec[] = { {1, 2}, {1, 2} };
    float aG[3] = {};

    aG[2] = 5.5;

    aVec[1].x = 9003;
    aVec[0].y = 46;

    short * pB = &b;
    *pB = 400;

    Vector2 vec2 = {};
    vec2.x = 50;
    vec2.y = 313;

    printf("vec = (%f, %f)\n", vec.x, vec.y);
    printf("vec2 = (%f, %f)\n", vec2.x, vec2.y);
    printf("aVec = [(%f, %f), (%f, %f)]\n", aVec[0].x, aVec[0].y, aVec[1].x, aVec[1].y);
    printf("aG = [%f, %f, %f]\n", aG[0], aG[1], aG[2]);
    

    printf("pi = %f\n", gPi);
    gPi = 3;
    printf("bad pi = %f\n", gPi);
    printf("a = %d %d %d\n", a, a, a);

    // BB (adrianb) Should error on i < c because types don't match...

	while (i < c)
    {
        printf("\"i\" = %d\n", a + *pB + -i);
        ++i;
    }

    printf("factorial(%lld) == %lld\n", 5ll, Factorial(5ll));

    return 0;
}