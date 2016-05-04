; ModuleID = 'builtin-module'
target triple = "x86_64-apple-macosx10.11.0"

%Vector2 = type { float, float }
%Sprite = type { [2 x %Vector2], %Vector2, float, i64 }

@vec = global %Vector2 { float 1.000000e+00, float 2.000000e+00 }
@gPi = global float 0x400921FA00000000
@0 = private unnamed_addr constant [23 x i8] c"vec = (%f, %f) dim %d\0A\00"
@1 = private unnamed_addr constant [17 x i8] c"vec2 = (%f, %f)\0A\00"
@2 = private unnamed_addr constant [29 x i8] c"aVec = [(%f, %f), (%f, %f)]\0A\00"
@3 = private unnamed_addr constant [100 x i8] c"aSprite = [([(%f, %f), (%f, %f)], (%f, %f), %f, %lld), ([(%f, %f), (%f, %f)], (%f, %f), %f, %lld)]\0A\00"
@4 = private unnamed_addr constant [19 x i8] c"aG = [%f, %f, %f]\0A\00"
@5 = private unnamed_addr constant [9 x i8] c"pi = %f\0A\00"
@6 = private unnamed_addr constant [13 x i8] c"bad pi = %f\0A\00"
@7 = private unnamed_addr constant [14 x i8] c"a = %d %d %d\0A\00"
@8 = private unnamed_addr constant [21 x i8] c"pos local is %f, %f\0A\00"
@9 = private unnamed_addr constant [10 x i8] c"\22i\22 = %d\0A\00"
@10 = private unnamed_addr constant [23 x i8] c"factorial(%ld) == %ld\0A\00"
@11 = private unnamed_addr constant [21 x i8] c"Leaving and pB = %d\0A\00"
@12 = private unnamed_addr constant [20 x i8] c"Made it past early\0A\00"
@13 = private unnamed_addr constant [21 x i8] c"Leaving and pB = %d\0A\00"

define i64 @Factorial(i64) {
entry:
  %1 = alloca i64
  store i64 %0, i64* %1
  %2 = load i64* %1
  %3 = icmp eq i64 %2, 0
  br i1 %3, label %ifpass, label %ifexit

ifpass:                                           ; preds = %entry
  ret i64 1

ifexit:                                           ; preds = %entry
  %4 = load i64* %1
  %5 = load i64* %1
  %6 = sub i64 %5, 1
  %7 = call i64 @Factorial(i64 %6)
  %8 = mul i64 %4, %7
  ret i64 %8
}

define void @main() {
entry:
  %n = alloca i32
  %i = alloca i8
  %c = alloca i64
  %b = alloca i16
  %aVec = alloca [2 x %Vector2]
  %aSprite = alloca [2 x %Sprite]
  %aG = alloca [3 x float]
  %pB = alloca i16*
  %vec2 = alloca %Vector2
  %pos = alloca %Vector2
  %p1 = alloca i32*
  %p2 = alloca i32*
  %p3 = alloca i32*
  %fEarly = alloca i1
  store i32 5, i32* %n
  store i8 0, i8* %i
  %0 = call i64 @CLoop()
  store i64 %0, i64* %c
  store i16 900, i16* %b
  %1 = getelementptr [2 x %Sprite]* %aSprite, i32 0, i64 1
  %2 = getelementptr inbounds %Sprite* %1, i32 0, i32 1
  %3 = getelementptr inbounds %Vector2* %2, i32 0, i32 0
  store float 5.500000e+00, float* %3
  %4 = getelementptr [2 x %Sprite]* %aSprite, i32 0, i64 1
  %5 = getelementptr inbounds %Sprite* %4, i32 0, i32 1
  %6 = getelementptr inbounds %Vector2* %5, i32 0, i32 1
  store float 1.050000e+01, float* %6
  %7 = getelementptr [2 x %Sprite]* %aSprite, i32 0, i64 1
  %8 = getelementptr inbounds %Sprite* %7, i32 0, i32 3
  store i64 98004, i64* %8
  %9 = getelementptr [2 x %Sprite]* %aSprite, i32 0, i64 0
  %10 = getelementptr inbounds %Sprite* %9, i32 0, i32 0
  %11 = getelementptr [2 x %Vector2]* %10, i32 0, i64 1
  %12 = getelementptr inbounds %Vector2* %11, i32 0, i32 1
  store float 0x403E666660000000, float* %12
  %13 = getelementptr [3 x float]* %aG, i32 0, i64 2
  store float 5.500000e+00, float* %13
  %14 = getelementptr [2 x %Vector2]* %aVec, i32 0, i64 1
  %15 = getelementptr inbounds %Vector2* %14, i32 0, i32 0
  store float 9.003000e+03, float* %15
  %16 = getelementptr [2 x %Vector2]* %aVec, i32 0, i64 0
  %17 = getelementptr inbounds %Vector2* %16, i32 0, i32 1
  store float 4.600000e+01, float* %17
  store i16* %b, i16** %pB
  %18 = load i16** %pB
  store i16 400, i16* %18
  %19 = getelementptr inbounds %Vector2* %vec2, i32 0, i32 0
  store float 5.000000e+01, float* %19
  %20 = getelementptr inbounds %Vector2* %vec2, i32 0, i32 1
  store float 3.130000e+02, float* %20
  %21 = load float* getelementptr inbounds (%Vector2* @vec, i32 0, i32 0)
  %22 = fpext float %21 to double
  %23 = load float* getelementptr inbounds (%Vector2* @vec, i32 0, i32 1)
  %24 = fpext float %23 to double
  %25 = call i32 (i8*, ...)* @printf(i8* getelementptr inbounds ([23 x i8]* @0, i32 0, i32 0), double %22, double %24, i32 2)
  %26 = getelementptr inbounds %Vector2* %vec2, i32 0, i32 0
  %27 = load float* %26
  %28 = fpext float %27 to double
  %29 = getelementptr inbounds %Vector2* %vec2, i32 0, i32 1
  %30 = load float* %29
  %31 = fpext float %30 to double
  %32 = call i32 (i8*, ...)* @printf(i8* getelementptr inbounds ([17 x i8]* @1, i32 0, i32 0), double %28, double %31)
  %33 = getelementptr [2 x %Vector2]* %aVec, i32 0, i64 0
  %34 = getelementptr inbounds %Vector2* %33, i32 0, i32 0
  %35 = load float* %34
  %36 = fpext float %35 to double
  %37 = getelementptr [2 x %Vector2]* %aVec, i32 0, i64 0
  %38 = getelementptr inbounds %Vector2* %37, i32 0, i32 1
  %39 = load float* %38
  %40 = fpext float %39 to double
  %41 = getelementptr [2 x %Vector2]* %aVec, i32 0, i64 1
  %42 = getelementptr inbounds %Vector2* %41, i32 0, i32 0
  %43 = load float* %42
  %44 = fpext float %43 to double
  %45 = getelementptr [2 x %Vector2]* %aVec, i32 0, i64 1
  %46 = getelementptr inbounds %Vector2* %45, i32 0, i32 1
  %47 = load float* %46
  %48 = fpext float %47 to double
  %49 = call i32 (i8*, ...)* @printf(i8* getelementptr inbounds ([29 x i8]* @2, i32 0, i32 0), double %36, double %40, double %44, double %48)
  %50 = getelementptr [2 x %Sprite]* %aSprite, i32 0, i64 0
  %51 = getelementptr inbounds %Sprite* %50, i32 0, i32 0
  %52 = getelementptr [2 x %Vector2]* %51, i32 0, i64 0
  %53 = getelementptr inbounds %Vector2* %52, i32 0, i32 0
  %54 = load float* %53
  %55 = fpext float %54 to double
  %56 = getelementptr [2 x %Sprite]* %aSprite, i32 0, i64 0
  %57 = getelementptr inbounds %Sprite* %56, i32 0, i32 0
  %58 = getelementptr [2 x %Vector2]* %57, i32 0, i64 0
  %59 = getelementptr inbounds %Vector2* %58, i32 0, i32 1
  %60 = load float* %59
  %61 = fpext float %60 to double
  %62 = getelementptr [2 x %Sprite]* %aSprite, i32 0, i64 0
  %63 = getelementptr inbounds %Sprite* %62, i32 0, i32 0
  %64 = getelementptr [2 x %Vector2]* %63, i32 0, i64 1
  %65 = getelementptr inbounds %Vector2* %64, i32 0, i32 0
  %66 = load float* %65
  %67 = fpext float %66 to double
  %68 = getelementptr [2 x %Sprite]* %aSprite, i32 0, i64 0
  %69 = getelementptr inbounds %Sprite* %68, i32 0, i32 0
  %70 = getelementptr [2 x %Vector2]* %69, i32 0, i64 1
  %71 = getelementptr inbounds %Vector2* %70, i32 0, i32 1
  %72 = load float* %71
  %73 = fpext float %72 to double
  %74 = getelementptr [2 x %Sprite]* %aSprite, i32 0, i64 0
  %75 = getelementptr inbounds %Sprite* %74, i32 0, i32 1
  %76 = getelementptr inbounds %Vector2* %75, i32 0, i32 0
  %77 = load float* %76
  %78 = fpext float %77 to double
  %79 = getelementptr [2 x %Sprite]* %aSprite, i32 0, i64 0
  %80 = getelementptr inbounds %Sprite* %79, i32 0, i32 1
  %81 = getelementptr inbounds %Vector2* %80, i32 0, i32 1
  %82 = load float* %81
  %83 = fpext float %82 to double
  %84 = getelementptr [2 x %Sprite]* %aSprite, i32 0, i64 0
  %85 = getelementptr inbounds %Sprite* %84, i32 0, i32 2
  %86 = load float* %85
  %87 = fpext float %86 to double
  %88 = getelementptr [2 x %Sprite]* %aSprite, i32 0, i64 0
  %89 = getelementptr inbounds %Sprite* %88, i32 0, i32 3
  %90 = load i64* %89
  %91 = getelementptr [2 x %Sprite]* %aSprite, i32 0, i64 1
  %92 = getelementptr inbounds %Sprite* %91, i32 0, i32 0
  %93 = getelementptr [2 x %Vector2]* %92, i32 0, i64 0
  %94 = getelementptr inbounds %Vector2* %93, i32 0, i32 0
  %95 = load float* %94
  %96 = fpext float %95 to double
  %97 = getelementptr [2 x %Sprite]* %aSprite, i32 0, i64 1
  %98 = getelementptr inbounds %Sprite* %97, i32 0, i32 0
  %99 = getelementptr [2 x %Vector2]* %98, i32 0, i64 0
  %100 = getelementptr inbounds %Vector2* %99, i32 0, i32 1
  %101 = load float* %100
  %102 = fpext float %101 to double
  %103 = getelementptr [2 x %Sprite]* %aSprite, i32 0, i64 1
  %104 = getelementptr inbounds %Sprite* %103, i32 0, i32 0
  %105 = getelementptr [2 x %Vector2]* %104, i32 0, i64 1
  %106 = getelementptr inbounds %Vector2* %105, i32 0, i32 0
  %107 = load float* %106
  %108 = fpext float %107 to double
  %109 = getelementptr [2 x %Sprite]* %aSprite, i32 0, i64 1
  %110 = getelementptr inbounds %Sprite* %109, i32 0, i32 0
  %111 = getelementptr [2 x %Vector2]* %110, i32 0, i64 1
  %112 = getelementptr inbounds %Vector2* %111, i32 0, i32 1
  %113 = load float* %112
  %114 = fpext float %113 to double
  %115 = getelementptr [2 x %Sprite]* %aSprite, i32 0, i64 1
  %116 = getelementptr inbounds %Sprite* %115, i32 0, i32 1
  %117 = getelementptr inbounds %Vector2* %116, i32 0, i32 0
  %118 = load float* %117
  %119 = fpext float %118 to double
  %120 = getelementptr [2 x %Sprite]* %aSprite, i32 0, i64 1
  %121 = getelementptr inbounds %Sprite* %120, i32 0, i32 1
  %122 = getelementptr inbounds %Vector2* %121, i32 0, i32 1
  %123 = load float* %122
  %124 = fpext float %123 to double
  %125 = getelementptr [2 x %Sprite]* %aSprite, i32 0, i64 1
  %126 = getelementptr inbounds %Sprite* %125, i32 0, i32 2
  %127 = load float* %126
  %128 = fpext float %127 to double
  %129 = getelementptr [2 x %Sprite]* %aSprite, i32 0, i64 1
  %130 = getelementptr inbounds %Sprite* %129, i32 0, i32 3
  %131 = load i64* %130
  %132 = call i32 (i8*, ...)* @printf(i8* getelementptr inbounds ([100 x i8]* @3, i32 0, i32 0), double %55, double %61, double %67, double %73, double %78, double %83, double %87, i64 %90, double %96, double %102, double %108, double %114, double %119, double %124, double %128, i64 %131)
  %133 = getelementptr [3 x float]* %aG, i32 0, i64 0
  %134 = load float* %133
  %135 = fpext float %134 to double
  %136 = getelementptr [3 x float]* %aG, i32 0, i64 1
  %137 = load float* %136
  %138 = fpext float %137 to double
  %139 = getelementptr [3 x float]* %aG, i32 0, i64 2
  %140 = load float* %139
  %141 = fpext float %140 to double
  %142 = call i32 (i8*, ...)* @printf(i8* getelementptr inbounds ([19 x i8]* @4, i32 0, i32 0), double %135, double %138, double %141)
  %143 = load float* @gPi
  %144 = fpext float %143 to double
  %145 = call i32 (i8*, ...)* @printf(i8* getelementptr inbounds ([9 x i8]* @5, i32 0, i32 0), double %144)
  store float 3.000000e+00, float* @gPi
  %146 = load float* @gPi
  %147 = fpext float %146 to double
  %148 = call i32 (i8*, ...)* @printf(i8* getelementptr inbounds ([13 x i8]* @6, i32 0, i32 0), double %147)
  %149 = call i32 (i8*, ...)* @printf(i8* getelementptr inbounds ([14 x i8]* @7, i32 0, i32 0), i32 2, i32 2, i32 2)
  %150 = getelementptr inbounds %Vector2* %pos, i32 0, i32 0
  store float 5.050000e+01, float* %150
  %151 = getelementptr inbounds %Vector2* %pos, i32 0, i32 1
  store float 0x407433AE20000000, float* %151
  %152 = getelementptr inbounds %Vector2* %pos, i32 0, i32 0
  %153 = load float* %152
  %154 = fpext float %153 to double
  %155 = getelementptr inbounds %Vector2* %pos, i32 0, i32 1
  %156 = load float* %155
  %157 = fpext float %156 to double
  %158 = call i32 (i8*, ...)* @printf(i8* getelementptr inbounds ([21 x i8]* @8, i32 0, i32 0), double %154, double %157)
  br label %whiletest

whiletest:                                        ; preds = %whilepass, %entry
  %159 = load i8* %i
  %160 = sext i8 %159 to i64
  %161 = load i64* %c
  %162 = icmp slt i64 %160, %161
  br i1 %162, label %whilepass, label %whileexit

whilepass:                                        ; preds = %whiletest
  %163 = load i16** %pB
  %164 = load i16* %163
  %165 = add i16 2, %164
  %166 = load i8* %i
  %167 = sub i8 0, %166
  %168 = sext i8 %167 to i16
  %169 = add i16 %165, %168
  %170 = sext i16 %169 to i32
  %171 = call i32 (i8*, ...)* @printf(i8* getelementptr inbounds ([10 x i8]* @9, i32 0, i32 0), i32 %170)
  %172 = load i8* %i
  %173 = add i8 %172, 1
  store i8 %173, i8* %i
  br label %whiletest

whileexit:                                        ; preds = %whiletest
  %174 = call i64 @CLoop()
  %175 = call i64 @CLoop()
  %176 = call i64 @Factorial(i64 %175)
  %177 = call i32 (i8*, ...)* @printf(i8* getelementptr inbounds ([23 x i8]* @10, i32 0, i32 0), i64 %174, i64 %176)
  store i32* null, i32** %p1
  %178 = load i32** %p1
  %179 = getelementptr i32* %178, i64 1
  store i32* %179, i32** %p2
  %180 = load i32** %p1
  %181 = getelementptr i32* %180, i64 1
  store i32* %181, i32** %p3
  %182 = load i32** %p2
  %183 = icmp eq i32* %182, null
  br i1 %183, label %andright, label %anddone

andright:                                         ; preds = %whileexit
  %184 = load i32** %p1
  %185 = icmp eq i32* %184, null
  br i1 %185, label %ordone, label %orright

orright:                                          ; preds = %andright
  %186 = load i32** %p3
  %187 = icmp eq i32* %186, null
  br label %ordone

ordone:                                           ; preds = %orright, %andright
  %188 = phi i1 [ true, %andright ], [ %187, %orright ]
  br label %anddone

anddone:                                          ; preds = %ordone, %whileexit
  %189 = phi i1 [ false, %whileexit ], [ %188, %ordone ]
  br i1 %189, label %ifpass, label %ifexit

ifpass:                                           ; preds = %anddone
  %190 = load i32** %p1
  store i32 3, i32* %190
  br label %ifexit

ifexit:                                           ; preds = %ifpass, %anddone
  store i1 true, i1* %fEarly
  %191 = load i1* %fEarly
  br i1 %191, label %ifpass1, label %ifexit2

ifpass1:                                          ; preds = %ifexit
  %192 = load i16** %pB
  %193 = load i16* %192
  %194 = sext i16 %193 to i32
  %195 = call i32 (i8*, ...)* @printf(i8* getelementptr inbounds ([21 x i8]* @11, i32 0, i32 0), i32 %194)
  ret void

ifexit2:                                          ; preds = %ifexit
  %196 = call i32 (i8*, ...)* @printf(i8* getelementptr inbounds ([20 x i8]* @12, i32 0, i32 0))
  store i16 302, i16* %b
  %197 = load i16** %pB
  %198 = load i16* %197
  %199 = sext i16 %198 to i32
  %200 = call i32 (i8*, ...)* @printf(i8* getelementptr inbounds ([21 x i8]* @13, i32 0, i32 0), i32 %199)
  ret void
}

define i64 @CLoop() {
entry:
  ret i64 5
}

declare i32 @printf(i8*, ...)
