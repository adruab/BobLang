; ModuleID = 'builtin-code'
target triple = "x86_64-apple-macosx10.11.0"

%Vector2 = type { float, float }
%Sprite = type { [2 x %Vector2], %Vector2, float, i64 }
%_StringStruct = type { i8*, i64 }

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
@.str = global [6 x i8] c"hello\00"
@11 = private unnamed_addr constant [30 x i8] c"string has %d characters %.*s\00"
@12 = private unnamed_addr constant [21 x i8] c"Leaving and pB = %d\0A\00"
@13 = private unnamed_addr constant [20 x i8] c"Made it past early\0A\00"
@14 = private unnamed_addr constant [21 x i8] c"Leaving and pB = %d\0A\00"

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
  %vec2p = alloca %Vector2*
  %pos = alloca %Vector2
  %p1 = alloca i32*
  %p2 = alloca i32*
  %p3 = alloca i32*
  %str = alloca %_StringStruct
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
  store %Vector2* %vec2, %Vector2** %vec2p
  %19 = load %Vector2** %vec2p
  %20 = getelementptr inbounds %Vector2* %19, i32 0, i32 0
  store float 5.000000e+01, float* %20
  %21 = load %Vector2** %vec2p
  %22 = getelementptr inbounds %Vector2* %21, i32 0, i32 1
  store float 3.130000e+02, float* %22
  %23 = load float* getelementptr inbounds (%Vector2* @vec, i32 0, i32 0)
  %24 = fpext float %23 to double
  %25 = load float* getelementptr inbounds (%Vector2* @vec, i32 0, i32 1)
  %26 = fpext float %25 to double
  %27 = call i32 (i8*, ...)* @printf(i8* getelementptr inbounds ([23 x i8]* @0, i32 0, i32 0), double %24, double %26, i32 2)
  %28 = getelementptr inbounds %Vector2* %vec2, i32 0, i32 0
  %29 = load float* %28
  %30 = fpext float %29 to double
  %31 = getelementptr inbounds %Vector2* %vec2, i32 0, i32 1
  %32 = load float* %31
  %33 = fpext float %32 to double
  %34 = call i32 (i8*, ...)* @printf(i8* getelementptr inbounds ([17 x i8]* @1, i32 0, i32 0), double %30, double %33)
  %35 = getelementptr [2 x %Vector2]* %aVec, i32 0, i64 0
  %36 = getelementptr inbounds %Vector2* %35, i32 0, i32 0
  %37 = load float* %36
  %38 = fpext float %37 to double
  %39 = getelementptr [2 x %Vector2]* %aVec, i32 0, i64 0
  %40 = getelementptr inbounds %Vector2* %39, i32 0, i32 1
  %41 = load float* %40
  %42 = fpext float %41 to double
  %43 = getelementptr [2 x %Vector2]* %aVec, i32 0, i64 1
  %44 = getelementptr inbounds %Vector2* %43, i32 0, i32 0
  %45 = load float* %44
  %46 = fpext float %45 to double
  %47 = getelementptr [2 x %Vector2]* %aVec, i32 0, i64 1
  %48 = getelementptr inbounds %Vector2* %47, i32 0, i32 1
  %49 = load float* %48
  %50 = fpext float %49 to double
  %51 = call i32 (i8*, ...)* @printf(i8* getelementptr inbounds ([29 x i8]* @2, i32 0, i32 0), double %38, double %42, double %46, double %50)
  %52 = getelementptr [2 x %Sprite]* %aSprite, i32 0, i64 0
  %53 = getelementptr inbounds %Sprite* %52, i32 0, i32 0
  %54 = getelementptr [2 x %Vector2]* %53, i32 0, i64 0
  %55 = getelementptr inbounds %Vector2* %54, i32 0, i32 0
  %56 = load float* %55
  %57 = fpext float %56 to double
  %58 = getelementptr [2 x %Sprite]* %aSprite, i32 0, i64 0
  %59 = getelementptr inbounds %Sprite* %58, i32 0, i32 0
  %60 = getelementptr [2 x %Vector2]* %59, i32 0, i64 0
  %61 = getelementptr inbounds %Vector2* %60, i32 0, i32 1
  %62 = load float* %61
  %63 = fpext float %62 to double
  %64 = getelementptr [2 x %Sprite]* %aSprite, i32 0, i64 0
  %65 = getelementptr inbounds %Sprite* %64, i32 0, i32 0
  %66 = getelementptr [2 x %Vector2]* %65, i32 0, i64 1
  %67 = getelementptr inbounds %Vector2* %66, i32 0, i32 0
  %68 = load float* %67
  %69 = fpext float %68 to double
  %70 = getelementptr [2 x %Sprite]* %aSprite, i32 0, i64 0
  %71 = getelementptr inbounds %Sprite* %70, i32 0, i32 0
  %72 = getelementptr [2 x %Vector2]* %71, i32 0, i64 1
  %73 = getelementptr inbounds %Vector2* %72, i32 0, i32 1
  %74 = load float* %73
  %75 = fpext float %74 to double
  %76 = getelementptr [2 x %Sprite]* %aSprite, i32 0, i64 0
  %77 = getelementptr inbounds %Sprite* %76, i32 0, i32 1
  %78 = getelementptr inbounds %Vector2* %77, i32 0, i32 0
  %79 = load float* %78
  %80 = fpext float %79 to double
  %81 = getelementptr [2 x %Sprite]* %aSprite, i32 0, i64 0
  %82 = getelementptr inbounds %Sprite* %81, i32 0, i32 1
  %83 = getelementptr inbounds %Vector2* %82, i32 0, i32 1
  %84 = load float* %83
  %85 = fpext float %84 to double
  %86 = getelementptr [2 x %Sprite]* %aSprite, i32 0, i64 0
  %87 = getelementptr inbounds %Sprite* %86, i32 0, i32 2
  %88 = load float* %87
  %89 = fpext float %88 to double
  %90 = getelementptr [2 x %Sprite]* %aSprite, i32 0, i64 0
  %91 = getelementptr inbounds %Sprite* %90, i32 0, i32 3
  %92 = load i64* %91
  %93 = getelementptr [2 x %Sprite]* %aSprite, i32 0, i64 1
  %94 = getelementptr inbounds %Sprite* %93, i32 0, i32 0
  %95 = getelementptr [2 x %Vector2]* %94, i32 0, i64 0
  %96 = getelementptr inbounds %Vector2* %95, i32 0, i32 0
  %97 = load float* %96
  %98 = fpext float %97 to double
  %99 = getelementptr [2 x %Sprite]* %aSprite, i32 0, i64 1
  %100 = getelementptr inbounds %Sprite* %99, i32 0, i32 0
  %101 = getelementptr [2 x %Vector2]* %100, i32 0, i64 0
  %102 = getelementptr inbounds %Vector2* %101, i32 0, i32 1
  %103 = load float* %102
  %104 = fpext float %103 to double
  %105 = getelementptr [2 x %Sprite]* %aSprite, i32 0, i64 1
  %106 = getelementptr inbounds %Sprite* %105, i32 0, i32 0
  %107 = getelementptr [2 x %Vector2]* %106, i32 0, i64 1
  %108 = getelementptr inbounds %Vector2* %107, i32 0, i32 0
  %109 = load float* %108
  %110 = fpext float %109 to double
  %111 = getelementptr [2 x %Sprite]* %aSprite, i32 0, i64 1
  %112 = getelementptr inbounds %Sprite* %111, i32 0, i32 0
  %113 = getelementptr [2 x %Vector2]* %112, i32 0, i64 1
  %114 = getelementptr inbounds %Vector2* %113, i32 0, i32 1
  %115 = load float* %114
  %116 = fpext float %115 to double
  %117 = getelementptr [2 x %Sprite]* %aSprite, i32 0, i64 1
  %118 = getelementptr inbounds %Sprite* %117, i32 0, i32 1
  %119 = getelementptr inbounds %Vector2* %118, i32 0, i32 0
  %120 = load float* %119
  %121 = fpext float %120 to double
  %122 = getelementptr [2 x %Sprite]* %aSprite, i32 0, i64 1
  %123 = getelementptr inbounds %Sprite* %122, i32 0, i32 1
  %124 = getelementptr inbounds %Vector2* %123, i32 0, i32 1
  %125 = load float* %124
  %126 = fpext float %125 to double
  %127 = getelementptr [2 x %Sprite]* %aSprite, i32 0, i64 1
  %128 = getelementptr inbounds %Sprite* %127, i32 0, i32 2
  %129 = load float* %128
  %130 = fpext float %129 to double
  %131 = getelementptr [2 x %Sprite]* %aSprite, i32 0, i64 1
  %132 = getelementptr inbounds %Sprite* %131, i32 0, i32 3
  %133 = load i64* %132
  %134 = call i32 (i8*, ...)* @printf(i8* getelementptr inbounds ([100 x i8]* @3, i32 0, i32 0), double %57, double %63, double %69, double %75, double %80, double %85, double %89, i64 %92, double %98, double %104, double %110, double %116, double %121, double %126, double %130, i64 %133)
  %135 = getelementptr [3 x float]* %aG, i32 0, i64 0
  %136 = load float* %135
  %137 = fpext float %136 to double
  %138 = getelementptr [3 x float]* %aG, i32 0, i64 1
  %139 = load float* %138
  %140 = fpext float %139 to double
  %141 = getelementptr [3 x float]* %aG, i32 0, i64 2
  %142 = load float* %141
  %143 = fpext float %142 to double
  %144 = call i32 (i8*, ...)* @printf(i8* getelementptr inbounds ([19 x i8]* @4, i32 0, i32 0), double %137, double %140, double %143)
  %145 = load float* @gPi
  %146 = fpext float %145 to double
  %147 = call i32 (i8*, ...)* @printf(i8* getelementptr inbounds ([9 x i8]* @5, i32 0, i32 0), double %146)
  store float 3.000000e+00, float* @gPi
  %148 = load float* @gPi
  %149 = fpext float %148 to double
  %150 = call i32 (i8*, ...)* @printf(i8* getelementptr inbounds ([13 x i8]* @6, i32 0, i32 0), double %149)
  %151 = call i32 (i8*, ...)* @printf(i8* getelementptr inbounds ([14 x i8]* @7, i32 0, i32 0), i32 2, i32 2, i32 2)
  %152 = getelementptr inbounds %Vector2* %pos, i32 0, i32 0
  store float 5.050000e+01, float* %152
  %153 = getelementptr inbounds %Vector2* %pos, i32 0, i32 1
  store float 0x407433AE20000000, float* %153
  %154 = getelementptr inbounds %Vector2* %pos, i32 0, i32 0
  %155 = load float* %154
  %156 = fpext float %155 to double
  %157 = getelementptr inbounds %Vector2* %pos, i32 0, i32 1
  %158 = load float* %157
  %159 = fpext float %158 to double
  %160 = call i32 (i8*, ...)* @printf(i8* getelementptr inbounds ([21 x i8]* @8, i32 0, i32 0), double %156, double %159)
  br label %whiletest

whiletest:                                        ; preds = %whilepass, %entry
  %161 = load i8* %i
  %162 = sext i8 %161 to i64
  %163 = load i64* %c
  %164 = icmp slt i64 %162, %163
  br i1 %164, label %whilepass, label %whileexit

whilepass:                                        ; preds = %whiletest
  %165 = load i16** %pB
  %166 = load i16* %165
  %167 = add i16 2, %166
  %168 = load i8* %i
  %169 = sub i8 0, %168
  %170 = sext i8 %169 to i16
  %171 = add i16 %167, %170
  %172 = sext i16 %171 to i32
  %173 = call i32 (i8*, ...)* @printf(i8* getelementptr inbounds ([10 x i8]* @9, i32 0, i32 0), i32 %172)
  %174 = load i8* %i
  %175 = add i8 %174, 1
  store i8 %175, i8* %i
  br label %whiletest

whileexit:                                        ; preds = %whiletest
  %176 = call i64 @CLoop()
  %177 = call i64 @CLoop()
  %178 = call i64 @Factorial(i64 %177)
  %179 = call i32 (i8*, ...)* @printf(i8* getelementptr inbounds ([23 x i8]* @10, i32 0, i32 0), i64 %176, i64 %178)
  store i32* null, i32** %p1
  %180 = load i32** %p1
  %181 = getelementptr i32* %180, i64 1
  store i32* %181, i32** %p2
  %182 = load i32** %p1
  %183 = getelementptr i32* %182, i64 1
  store i32* %183, i32** %p3
  %184 = load i32** %p2
  %185 = icmp eq i32* %184, null
  br i1 %185, label %andright, label %anddone

andright:                                         ; preds = %whileexit
  %186 = load i32** %p1
  %187 = icmp eq i32* %186, null
  br i1 %187, label %ordone, label %orright

orright:                                          ; preds = %andright
  %188 = load i32** %p3
  %189 = icmp eq i32* %188, null
  br label %ordone

ordone:                                           ; preds = %orright, %andright
  %190 = phi i1 [ true, %andright ], [ %189, %orright ]
  br label %anddone

anddone:                                          ; preds = %ordone, %whileexit
  %191 = phi i1 [ false, %whileexit ], [ %190, %ordone ]
  br i1 %191, label %ifpass, label %ifexit

ifpass:                                           ; preds = %anddone
  %192 = load i32** %p1
  store i32 3, i32* %192
  br label %ifexit

ifexit:                                           ; preds = %ifpass, %anddone
  store %_StringStruct { i8* getelementptr inbounds ([6 x i8]* @.str, i32 0, i32 0), i64 5 }, %_StringStruct* %str
  %193 = getelementptr inbounds %_StringStruct* %str, i32 0, i32 1
  %194 = load i64* %193
  %195 = getelementptr inbounds %_StringStruct* %str, i32 0, i32 1
  %196 = load i64* %195
  %197 = getelementptr inbounds %_StringStruct* %str, i32 0, i32 0
  %198 = load i8** %197
  %199 = call i32 (i8*, ...)* @printf(i8* getelementptr inbounds ([30 x i8]* @11, i32 0, i32 0), i64 %194, i64 %196, i8* %198)
  store i1 true, i1* %fEarly
  %200 = load i1* %fEarly
  br i1 %200, label %ifpass1, label %ifexit2

ifpass1:                                          ; preds = %ifexit
  %201 = load i16** %pB
  %202 = load i16* %201
  %203 = sext i16 %202 to i32
  %204 = call i32 (i8*, ...)* @printf(i8* getelementptr inbounds ([21 x i8]* @12, i32 0, i32 0), i32 %203)
  ret void

ifexit2:                                          ; preds = %ifexit
  %205 = call i32 (i8*, ...)* @printf(i8* getelementptr inbounds ([20 x i8]* @13, i32 0, i32 0))
  store i16 302, i16* %b
  %206 = load i16** %pB
  %207 = load i16* %206
  %208 = sext i16 %207 to i32
  %209 = call i32 (i8*, ...)* @printf(i8* getelementptr inbounds ([21 x i8]* @14, i32 0, i32 0), i32 %208)
  ret void
}

define i64 @CLoop() {
entry:
  ret i64 5
}

declare i32 @printf(i8*, ...)
